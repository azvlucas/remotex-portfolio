// RemoteX — host/backup_agent.cpp
// Agente de backup agendado com manifesto local, compressão e upload.

#include "host/backup_agent.h"
#include "core/crypto_utils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

namespace remotex::host {

// ─── JSON helpers mínimos ───────────────────────────────────────────────────

static std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";
    if (json[pos] == '"') {
        auto end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        return json.substr(pos + 1, end - pos - 1);
    }
    auto end = json.find_first_of(",} \t\n\r", pos);
    return json.substr(pos, end - pos);
}

static bool JsonGetBool(const std::string& json, const std::string& key) {
    return JsonGetString(json, key) == "true";
}

static int64_t JsonGetInt(const std::string& json, const std::string& key) {
    auto val = JsonGetString(json, key);
    if (val.empty()) return 0;
    try { return std::stoll(val); } catch (...) { return 0; }
}

// Extrai um array JSON de strings: ["a","b","c"]
static std::vector<std::string> JsonGetStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos + search.size());
    if (pos == std::string::npos) return result;
    auto end = json.find(']', pos);
    if (end == std::string::npos) return result;

    std::string arr = json.substr(pos + 1, end - pos - 1);
    size_t i = 0;
    while (i < arr.size()) {
        auto q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        auto q2 = q1 + 1;
        while (q2 < arr.size()) {
            if (arr[q2] == '\\') { q2 += 2; continue; }
            if (arr[q2] == '"') break;
            ++q2;
        }
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), len);
    return ws;
}

// ─── Construtor / Destrutor ─────────────────────────────────────────────────

BackupAgent::BackupAgent() = default;

BackupAgent::~BackupAgent() {
    Stop();
}

// ─── Initialize ─────────────────────────────────────────────────────────────

void BackupAgent::Initialize(const Config& config, remotex::core::AuthClient* auth) {
    config_ = config;
    auth_   = auth;

    // Garante que os diretórios existem.
    std::error_code ec;
    std::filesystem::create_directories(config_.manifest_path.parent_path(), ec);
    std::filesystem::create_directories(config_.temp_dir, ec);
}

void BackupAgent::SetListener(BackupAgentListener* listener) {
    listener_ = listener;
}

// ─── Start / Stop ───────────────────────────────────────────────────────────

void BackupAgent::Start() {
    if (running_)
        return;

    should_stop_ = false;
    running_     = true;
    thread_      = std::thread(&BackupAgent::SchedulerLoop, this);
}

void BackupAgent::Stop() {
    should_stop_ = true;
    if (thread_.joinable())
        thread_.join();
    running_ = false;
}

// ─── SchedulerLoop ──────────────────────────────────────────────────────────

void BackupAgent::SchedulerLoop() {
    while (!should_stop_) {
        // Buscar config do backend.
        BackupConfig bc;
        if (!FetchConfig(bc) || !bc.enabled) {
            // Se não conseguiu ou desabilitado, espera 5 minutos e tenta de novo.
            for (uint32_t elapsed = 0; elapsed < 300000 && !should_stop_; elapsed += 500)
                Sleep(500);
            continue;
        }

        // Executar o ciclo de backup.
        auto result = RunBackupCycle();

        if (listener_)
            listener_->OnBackupCycleComplete(result);

        // Dormir pelo intervalo configurado (em horas).
        uint32_t wait_ms = bc.interval_hours * 3600 * 1000;
        for (uint32_t elapsed = 0; elapsed < wait_ms && !should_stop_; elapsed += 500)
            Sleep(500);
    }
}

// ─── RunBackupCycle ─────────────────────────────────────────────────────────

BackupCycleResult BackupAgent::RunBackupCycle() {
    BackupCycleResult result;
    auto start_time = std::chrono::steady_clock::now();

    // 1. Buscar config do backend.
    BackupConfig bc;
    if (!FetchConfig(bc)) {
        result.error = "failed to fetch backup config from backend";
        return result;
    }

    if (!bc.enabled || bc.folders.empty()) {
        result.success = true;
        return result;
    }

    // 2. Carregar manifesto local.
    auto manifest = LoadManifest();

    // 3. Escanear pastas configuradas.
    auto files = ScanFolders(bc.folders);

    // 4. Comparar com manifesto — identificar novos, alterados, removidos.
    std::vector<std::filesystem::path> files_to_backup;
    Manifest new_manifest;
    std::map<std::string, bool> seen;

    for (const auto& fi : files) {
        std::string path_str = fi.path.string();
        std::string sha = FileSHA256(fi.path);
        if (sha.empty()) continue;  // Não conseguiu ler o arquivo.

        ManifestEntry entry;
        entry.sha256   = sha;
        entry.size     = fi.size;
        entry.modified = fi.modified;
        new_manifest[path_str] = entry;
        seen[path_str] = true;

        auto it = manifest.find(path_str);
        if (it == manifest.end()) {
            // Arquivo novo.
            files_to_backup.push_back(fi.path);
            result.files_new++;
        } else if (it->second.sha256 != sha) {
            // Arquivo alterado.
            files_to_backup.push_back(fi.path);
            result.files_changed++;
        }
    }

    // Contar removidos.
    for (const auto& [path, _] : manifest) {
        if (!seen.count(path))
            result.files_removed++;
    }

    // 5. Se não há nada para fazer, sucesso.
    if (files_to_backup.empty()) {
        SaveManifest(new_manifest);
        result.success = true;
        auto end_time = std::chrono::steady_clock::now();
        result.duration_sec = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(
            end_time - start_time).count();
        return result;
    }

    // 6. Compactar em .tar.gz
    auto archive_path = CreateArchive(files_to_backup);
    if (archive_path.empty()) {
        result.error = "failed to create archive";
        return result;
    }

    // Obter tamanho do arquivo.
    std::error_code ec;
    auto archive_size = std::filesystem::file_size(archive_path, ec);
    if (ec) {
        result.error = "failed to get archive size";
        std::filesystem::remove(archive_path, ec);
        return result;
    }
    result.archive_size = archive_size;

    // 7. Solicitar URL pré-assinada.
    std::string upload_url, storage_key;
    if (!RequestUploadURL(archive_size, (uint32_t)files_to_backup.size(),
                          upload_url, storage_key)) {
        result.error = "failed to get presigned upload URL";
        std::filesystem::remove(archive_path, ec);
        return result;
    }

    // 8. Upload do .tar.gz.
    if (!UploadFile(upload_url, archive_path)) {
        result.error = "upload failed";
        std::filesystem::remove(archive_path, ec);
        return result;
    }

    // 9. Remover arquivo temporário.
    std::filesystem::remove(archive_path, ec);

    // 10. Atualizar manifesto local.
    SaveManifest(new_manifest);

    auto end_time = std::chrono::steady_clock::now();
    result.duration_sec = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time).count();
    result.success = true;
    return result;
}

// ─── Manifesto ──────────────────────────────────────────────────────────────

BackupAgent::Manifest BackupAgent::LoadManifest() {
    Manifest manifest;
    std::ifstream file(config_.manifest_path, std::ios::binary);
    if (!file.is_open()) return manifest;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    // Parsear JSON manualmente: { "last_backup":"...", "files": { "path": { "sha256":"...", "size":N, "modified":"..." }, ... } }
    // Encontrar o bloco "files": { ... }
    auto files_pos = content.find("\"files\"");
    if (files_pos == std::string::npos) return manifest;

    auto brace_start = content.find('{', files_pos + 7);
    if (brace_start == std::string::npos) return manifest;

    // Encontrar o } correspondente.
    int depth = 0;
    size_t brace_end = brace_start;
    for (size_t i = brace_start; i < content.size(); ++i) {
        if (content[i] == '{') ++depth;
        else if (content[i] == '}') {
            --depth;
            if (depth == 0) { brace_end = i; break; }
        }
    }

    std::string files_block = content.substr(brace_start + 1, brace_end - brace_start - 1);

    // Extrair pares key: { sha256, size, modified }
    size_t pos = 0;
    while (pos < files_block.size()) {
        // Encontrar a chave (path).
        auto q1 = files_block.find('"', pos);
        if (q1 == std::string::npos) break;
        auto q2 = q1 + 1;
        while (q2 < files_block.size()) {
            if (files_block[q2] == '\\') { q2 += 2; continue; }
            if (files_block[q2] == '"') break;
            ++q2;
        }
        std::string path_key = files_block.substr(q1 + 1, q2 - q1 - 1);

        // Encontrar o bloco { ... } do valor.
        auto obj_start = files_block.find('{', q2 + 1);
        if (obj_start == std::string::npos) break;
        auto obj_end = files_block.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj = files_block.substr(obj_start, obj_end - obj_start + 1);
        ManifestEntry entry;
        entry.sha256   = JsonGetString(obj, "sha256");
        entry.size     = (uint64_t)JsonGetInt(obj, "size");
        entry.modified = JsonGetString(obj, "modified");

        manifest[path_key] = entry;
        pos = obj_end + 1;
    }

    return manifest;
}

void BackupAgent::SaveManifest(const Manifest& manifest) {
    std::ofstream file(config_.manifest_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return;

    // Gerar timestamp ISO 8601 atual.
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    gmtime_s(&tm_buf, &time_t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    file << "{\n";
    file << "  \"last_backup\": \"" << time_str << "\",\n";
    file << "  \"files\": {\n";

    bool first = true;
    for (const auto& [path, entry] : manifest) {
        if (!first) file << ",\n";
        first = false;
        file << "    \"" << EscapeJson(path) << "\": {\n";
        file << "      \"sha256\": \"" << entry.sha256 << "\",\n";
        file << "      \"size\": " << entry.size << ",\n";
        file << "      \"modified\": \"" << entry.modified << "\"\n";
        file << "    }";
    }

    file << "\n  }\n";
    file << "}\n";
}

// ─── Scanning ───────────────────────────────────────────────────────────────

std::vector<BackupAgent::FileInfo> BackupAgent::ScanFolders(const std::vector<std::string>& folders) {
    std::vector<FileInfo> result;

    for (const auto& folder : folders) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file(ec)) continue;
            if (ec) { ec.clear(); continue; }

            FileInfo fi;
            fi.path     = entry.path();
            fi.size     = entry.file_size(ec);
            if (ec) { ec.clear(); continue; }
            fi.modified = TimeToISO8601(entry.last_write_time(ec));
            if (ec) { ec.clear(); continue; }

            result.push_back(std::move(fi));
        }
    }

    return result;
}

std::string BackupAgent::FileSHA256(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";

    // Ler em blocos de 64KB e calcular SHA-256 incremental via BCrypt.
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return "";

    status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return "";
    }

    constexpr size_t kBufSize = 64 * 1024;
    std::vector<uint8_t> buf(kBufSize);
    while (file.good()) {
        file.read(reinterpret_cast<char*>(buf.data()), kBufSize);
        auto bytes_read = file.gcount();
        if (bytes_read > 0) {
            BCryptHashData(hash, buf.data(), (ULONG)bytes_read, 0);
        }
    }

    uint8_t digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    // Converter para hex.
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; ++i) {
        result += hex_chars[digest[i] >> 4];
        result += hex_chars[digest[i] & 0x0F];
    }
    return result;
}

std::string BackupAgent::TimeToISO8601(std::filesystem::file_time_type tp) {
    // Converter file_time_type para system_clock::time_point.
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(tp);
    auto time_t = std::chrono::system_clock::to_time_t(sctp);
    struct tm tm_buf;
    gmtime_s(&tm_buf, &time_t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// ─── Compressão (minizip via zlib deflate — gzip de um tar simples) ─────────
// Sem libarchive: criamos um .tar básico manualmente + gzip via zlib.
// O formato tar POSIX (ustar) é simples o suficiente para criar manualmente.

// Estrutura do header tar (512 bytes).
struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};
static_assert(sizeof(TarHeader) == 512, "TarHeader must be 512 bytes");

static void WriteTarOctal(char* dest, size_t width, uint64_t value) {
    // Escreve em octal com leading zeros, sem null terminator no último char.
    std::string s;
    if (value == 0) {
        s = "0";
    } else {
        while (value > 0) {
            s = char('0' + (value & 7)) + s;
            value >>= 3;
        }
    }
    // Pad com zeros à esquerda (width-1 chars + null).
    size_t field_width = width - 1;
    size_t pad = (s.size() < field_width) ? (field_width - s.size()) : 0;
    size_t offset = 0;
    for (size_t i = 0; i < pad && offset < width; ++i) dest[offset++] = '0';
    for (size_t i = 0; i < s.size() && offset < width; ++i) dest[offset++] = s[i];
    if (offset < width) dest[offset] = '\0';
}

static uint32_t TarChecksum(const TarHeader& hdr) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&hdr);
    uint32_t sum = 0;
    for (int i = 0; i < 512; ++i) {
        // O campo checksum (offset 148, 8 bytes) é tratado como espaços.
        if (i >= 148 && i < 156) sum += ' ';
        else sum += bytes[i];
    }
    return sum;
}

// Deflateia dados brutos para gzip.
// Usamos a API de compressão do Windows (RtlCompressBuffer não suporta gzip).
// Implementamos gzip manualmente com a API deflate do zlib se disponível.
// Fallback: usamos compressão via WinAPI Cabinet (sem dependência externa).
// Na prática, como o projeto já linka zlib condicionalmente, usamos
// compressão direta no formato tar sem gzip quando zlib não está disponível.
// O .tar pode ser grande mas funcional.

// Para simplicidade e zero dependências extras, implementamos gzip usando
// a Windows Compression API (COMPRESSION_FORMAT_LZNT1/XPRESS/XPRESS_HUFF).
// Porém, como o backend espera .tar.gz, usamos gzip real via deflate2.
// Solução: escrever o .tar e compactar via gzip com a inflate/deflate do bcrypt?
// Não — BCrypt não tem deflate.
//
// Abordagem final: o .tar é criado sem compressão. O agente usa WinHTTP
// com Content-Encoding: gzip OU simplesmente envia o .tar sem compressão,
// uma vez que a URL pré-assinada aceita qualquer conteúdo.
// O backend registra como .tar.gz por convenção do storage key,
// mas o conteúdo pode ser um .tar plano.
// Se zlib estiver disponível no build, aplicamos gzip. Caso contrário, .tar puro.

// Forward declarations para compressão condicional.
#ifdef REMOTEX_HAS_ZLIB
#include <zlib.h>
static bool GzipCompressFile(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::ifstream in(input, std::ios::binary);
    if (!in.is_open()) return false;

    gzFile gz = gzopen(output.string().c_str(), "wb9");
    if (!gz) return false;

    char buf[65536];
    while (in.good()) {
        in.read(buf, sizeof(buf));
        auto n = in.gcount();
        if (n > 0) gzwrite(gz, buf, (unsigned)n);
    }
    gzclose(gz);
    return true;
}
#endif

std::filesystem::path BackupAgent::CreateArchive(const std::vector<std::filesystem::path>& files) {
    auto tar_path = config_.temp_dir / "backup.tar";

    std::ofstream tar(tar_path, std::ios::binary | std::ios::trunc);
    if (!tar.is_open()) return {};

    for (const auto& fpath : files) {
        // Ler o arquivo.
        std::ifstream in(fpath, std::ios::binary);
        if (!in.is_open()) continue;

        // Obter tamanho.
        in.seekg(0, std::ios::end);
        auto file_size = in.tellg();
        in.seekg(0, std::ios::beg);

        // Criar header tar.
        TarHeader hdr{};
        std::string name = fpath.string();
        // Substituir '\' por '/' para compatibilidade.
        std::replace(name.begin(), name.end(), '\\', '/');
        // Remover leading '/' ou 'C:/' etc.
        if (name.size() >= 3 && name[1] == ':' && name[2] == '/')
            name = name.substr(3);
        else if (!name.empty() && name[0] == '/')
            name = name.substr(1);

        // Se o nome cabe em 100 chars, usar name. Senão, usar prefix+name.
        if (name.size() < 100) {
            std::memcpy(hdr.name, name.c_str(), name.size());
        } else if (name.size() < 255) {
            // Split em prefix (até 155) e name (até 100).
            auto slash = name.rfind('/', 154);
            if (slash != std::string::npos) {
                std::memcpy(hdr.prefix, name.c_str(), slash);
                std::memcpy(hdr.name, name.c_str() + slash + 1, name.size() - slash - 1);
            } else {
                std::memcpy(hdr.name, name.c_str(), 99);
            }
        } else {
            // Nome muito longo, truncar.
            std::memcpy(hdr.name, name.c_str() + name.size() - 99, 99);
        }

        WriteTarOctal(hdr.mode, 8, 0644);
        WriteTarOctal(hdr.uid, 8, 0);
        WriteTarOctal(hdr.gid, 8, 0);
        WriteTarOctal(hdr.size, 12, (uint64_t)file_size);

        // mtime: seconds since epoch.
        std::error_code ec;
        auto lwt = std::filesystem::last_write_time(fpath, ec);
        if (!ec) {
            auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(lwt);
            auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                sctp.time_since_epoch()).count();
            WriteTarOctal(hdr.mtime, 12, (uint64_t)epoch);
        }

        hdr.typeflag = '0';  // Regular file.
        std::memcpy(hdr.magic, "ustar", 5);
        hdr.magic[5] = '\0';
        hdr.version[0] = '0'; hdr.version[1] = '0';

        // Calcular checksum.
        std::memset(hdr.checksum, ' ', 8);
        uint32_t cksum = TarChecksum(hdr);
        WriteTarOctal(hdr.checksum, 7, cksum);
        hdr.checksum[7] = ' ';

        // Escrever header.
        tar.write(reinterpret_cast<const char*>(&hdr), 512);

        // Escrever dados do arquivo.
        constexpr size_t kBufSize = 65536;
        std::vector<char> buf(kBufSize);
        uint64_t written = 0;
        while (in.good() && written < (uint64_t)file_size) {
            in.read(buf.data(), kBufSize);
            auto n = in.gcount();
            if (n > 0) {
                tar.write(buf.data(), n);
                written += n;
            }
        }

        // Padding para 512 bytes.
        auto remainder = written % 512;
        if (remainder != 0) {
            std::vector<char> padding(512 - remainder, 0);
            tar.write(padding.data(), (std::streamsize)padding.size());
        }
    }

    // End-of-archive: dois blocos de 512 bytes zerados.
    std::vector<char> end_marker(1024, 0);
    tar.write(end_marker.data(), 1024);
    tar.close();

#ifdef REMOTEX_HAS_ZLIB
    auto gz_path = config_.temp_dir / "backup.tar.gz";
    if (GzipCompressFile(tar_path, gz_path)) {
        std::error_code ec;
        std::filesystem::remove(tar_path, ec);
        return gz_path;
    }
#endif

    // Sem zlib, retorna o .tar diretamente.
    return tar_path;
}

// ─── HTTP helpers ───────────────────────────────────────────────────────────

bool BackupAgent::FetchConfig(BackupConfig& out) {
    auto resp = HttpRequest("GET", "/backup/config/" + config_.machine_id);
    if (resp.status_code != 200) return false;

    out.folders         = JsonGetStringArray(resp.body, "folders");
    out.interval_hours  = (uint32_t)JsonGetInt(resp.body, "interval_hours");
    out.retention_count = (uint32_t)JsonGetInt(resp.body, "retention_count");
    out.enabled         = JsonGetBool(resp.body, "enabled");

    if (out.interval_hours == 0) out.interval_hours = 2;
    if (out.retention_count == 0) out.retention_count = 7;
    return true;
}

bool BackupAgent::RequestUploadURL(uint64_t size_bytes, uint32_t file_count,
                                    std::string& out_url, std::string& out_storage_key) {
    std::ostringstream body;
    body << "{\"size_bytes\":" << size_bytes << ",\"file_count\":" << file_count << "}";

    auto resp = HttpRequest("POST", "/backup/upload/" + config_.machine_id, body.str());
    if (resp.status_code != 200) return false;

    out_url         = JsonGetString(resp.body, "url");
    out_storage_key = JsonGetString(resp.body, "storage_key");
    return !out_url.empty();
}

bool BackupAgent::UploadFile(const std::string& url, const std::filesystem::path& file_path) {
    auto resp = HttpPut(url, file_path);
    return resp.status_code >= 200 && resp.status_code < 300;
}

bool BackupAgent::RequestDeleteOldest() {
    // O backend já faz enforcement de retenção no endpoint de upload.
    // Este método existe para chamada explícita se necessário.
    return true;
}

// ─── HTTP via WinHTTP (para API backend) ────────────────────────────────────

BackupAgent::HttpResponse BackupAgent::HttpRequest(const std::string& method,
                                                    const std::string& path,
                                                    const std::string& json_body) {
    HttpResponse result;

    HINTERNET session = WinHttpOpen(L"RemoteX-Backup/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    auto wide_host = ToWide(config_.api_host);
    HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), config_.api_port, 0);
    if (!conn) { WinHttpCloseHandle(session); return result; }

    auto wide_path = ToWide(path);
    auto wide_method = ToWide(method);
    DWORD flags = config_.use_tls ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET req = WinHttpOpenRequest(conn, wide_method.c_str(), wide_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

    // Headers.
    WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);

    // Authorization Bearer token.
    if (auth_) {
        std::string token = auth_->GetAccessToken();
        if (!token.empty()) {
            std::wstring auth_header = L"Authorization: Bearer " + ToWide(token);
            WinHttpAddRequestHeaders(req, auth_header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD);
        }
    }

    // Send.
    LPVOID body_ptr = json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data();
    DWORD body_len = (DWORD)json_body.size();

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body_ptr, body_len, body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    if (ok) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        result.status_code = static_cast<int>(status);

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk(avail, 0);
            DWORD read = 0;
            WinHttpReadData(req, chunk.data(), avail, &read);
            result.body.append(chunk.data(), read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

// ─── HTTP PUT para URL absoluta (presigned URL do R2) ────────────────────────

BackupAgent::HttpResponse BackupAgent::HttpPut(const std::string& url,
                                                const std::filesystem::path& file_path) {
    HttpResponse result;

    // Parsear a URL para extrair host, path, porta, TLS.
    // Formato: https://host:port/path?query
    std::string host, url_path;
    uint16_t port = 443;
    bool use_tls = true;

    std::string work = url;
    if (work.substr(0, 8) == "https://") {
        work = work.substr(8);
        use_tls = true;
        port = 443;
    } else if (work.substr(0, 7) == "http://") {
        work = work.substr(7);
        use_tls = false;
        port = 80;
    }

    auto slash_pos = work.find('/');
    if (slash_pos != std::string::npos) {
        host = work.substr(0, slash_pos);
        url_path = work.substr(slash_pos);
    } else {
        host = work;
        url_path = "/";
    }

    // Checar se host contém porta.
    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        try { port = (uint16_t)std::stoi(host.substr(colon_pos + 1)); } catch (...) {}
        host = host.substr(0, colon_pos);
    }

    // Ler o arquivo para upload.
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return result;
    file.seekg(0, std::ios::end);
    auto file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> file_data((size_t)file_size);
    file.read(file_data.data(), file_size);
    file.close();

    // WinHTTP.
    HINTERNET session = WinHttpOpen(L"RemoteX-Backup/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    auto wide_host = ToWide(host);
    HINTERNET conn = WinHttpConnect(session, wide_host.c_str(), port, 0);
    if (!conn) { WinHttpCloseHandle(session); return result; }

    auto wide_path = ToWide(url_path);
    DWORD flags = use_tls ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET req = WinHttpOpenRequest(conn, L"PUT", wide_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return result; }

    WinHttpAddRequestHeaders(req, L"Content-Type: application/gzip", (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        file_data.data(), (DWORD)file_data.size(), (DWORD)file_data.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    if (ok) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        result.status_code = static_cast<int>(status);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

} // namespace remotex::host
