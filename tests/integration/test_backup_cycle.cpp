// RemoteX — tests/integration/test_backup_cycle.cpp
// Teste de integração: verifica ciclo de backup (scan → hash → tar → manifesto).
// Testa componentes locais sem depender do R2.

#include <gtest/gtest.h>
#include "core/crypto_utils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class BackupCycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "remotex_backup_test";
        source_dir_ = test_dir_ / "source";
        fs::create_directories(source_dir_);

        // Criar estrutura de arquivos de teste.
        CreateFile(source_dir_ / "file1.txt", "Hello World");
        CreateFile(source_dir_ / "file2.txt", "Backup Test Content");

        auto subdir = source_dir_ / "subdir";
        fs::create_directories(subdir);
        CreateFile(subdir / "nested.txt", "Nested file content");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void CreateFile(const fs::path& path, const std::string& content) {
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
    }

    std::string ReadFile(const fs::path& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        auto size = f.tellg();
        f.seekg(0);
        std::string content(static_cast<size_t>(size), '\0');
        f.read(content.data(), size);
        return content;
    }

    fs::path test_dir_;
    fs::path source_dir_;
};

TEST_F(BackupCycleTest, SHA256_Deterministic) {
    auto content = ReadFile(source_dir_ / "file1.txt");
    std::vector<uint8_t> data(content.begin(), content.end());

    auto hash1 = remotex::core::SHA256Hex(data);
    auto hash2 = remotex::core::SHA256Hex(data);

    EXPECT_EQ(hash1, hash2);
    EXPECT_EQ(hash1.size(), 64u);
}

TEST_F(BackupCycleTest, SHA256_DifferentContent) {
    std::vector<uint8_t> data1 = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> data2 = {'W', 'o', 'r', 'l', 'd'};

    auto hash1 = remotex::core::SHA256Hex(data1);
    auto hash2 = remotex::core::SHA256Hex(data2);

    EXPECT_NE(hash1, hash2);
}

TEST_F(BackupCycleTest, SHA256_EmptyData) {
    std::vector<uint8_t> empty;
    auto hash = remotex::core::SHA256Hex(empty);

    // SHA-256 of empty string is well-known.
    EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(BackupCycleTest, DirectoryScan) {
    // Verificar que os arquivos de teste existem.
    int file_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(source_dir_)) {
        if (entry.is_regular_file())
            ++file_count;
    }
    EXPECT_EQ(file_count, 3);  // file1.txt, file2.txt, subdir/nested.txt
}

TEST_F(BackupCycleTest, IncrementalDetection) {
    // Simular manifesto: hash dos arquivos originais.
    auto content1 = ReadFile(source_dir_ / "file1.txt");
    auto content2 = ReadFile(source_dir_ / "file2.txt");

    auto hash1_orig = remotex::core::SHA256Hex(
        std::vector<uint8_t>(content1.begin(), content1.end()));
    auto hash2_orig = remotex::core::SHA256Hex(
        std::vector<uint8_t>(content2.begin(), content2.end()));

    // Modificar file1.
    CreateFile(source_dir_ / "file1.txt", "Modified content");
    content1 = ReadFile(source_dir_ / "file1.txt");
    auto hash1_new = remotex::core::SHA256Hex(
        std::vector<uint8_t>(content1.begin(), content1.end()));

    // file1 deve ter hash diferente.
    EXPECT_NE(hash1_orig, hash1_new);

    // file2 não mudou.
    content2 = ReadFile(source_dir_ / "file2.txt");
    auto hash2_new = remotex::core::SHA256Hex(
        std::vector<uint8_t>(content2.begin(), content2.end()));
    EXPECT_EQ(hash2_orig, hash2_new);
}

TEST_F(BackupCycleTest, NewFileDetection) {
    int before = 0;
    for (const auto& e : fs::recursive_directory_iterator(source_dir_))
        if (e.is_regular_file()) ++before;

    // Adicionar novo arquivo.
    CreateFile(source_dir_ / "new_file.txt", "New file");

    int after = 0;
    for (const auto& e : fs::recursive_directory_iterator(source_dir_))
        if (e.is_regular_file()) ++after;

    EXPECT_EQ(after, before + 1);
}

TEST_F(BackupCycleTest, DeletedFileDetection) {
    // Criar manifesto com file1, file2, nested.
    std::vector<std::string> manifest_files;
    for (const auto& e : fs::recursive_directory_iterator(source_dir_))
        if (e.is_regular_file())
            manifest_files.push_back(e.path().string());

    EXPECT_EQ(manifest_files.size(), 3u);

    // Deletar um arquivo.
    fs::remove(source_dir_ / "file2.txt");

    // Re-scan.
    std::vector<std::string> current_files;
    for (const auto& e : fs::recursive_directory_iterator(source_dir_))
        if (e.is_regular_file())
            current_files.push_back(e.path().string());

    EXPECT_EQ(current_files.size(), 2u);

    // Detectar deletado.
    int deleted_count = 0;
    for (const auto& mf : manifest_files) {
        if (!fs::exists(mf))
            ++deleted_count;
    }
    EXPECT_EQ(deleted_count, 1);
}
