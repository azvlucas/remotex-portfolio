package admin

const sharedCSS = `
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{font-family:"Inter","Segoe UI",sans-serif;background:#f1f5f9;color:#1e293b;min-height:100vh}
a{color:inherit;text-decoration:none}
.layout{display:flex;min-height:100vh}
.sidebar{width:240px;background:#0c1929;color:#94a3b8;position:fixed;top:0;left:0;bottom:0;display:flex;flex-direction:column;z-index:100}
.sidebar .logo{padding:24px 20px 20px;font-size:22px;font-weight:800;color:#fff;letter-spacing:-0.5px}
.sidebar .logo span{color:#3b82f6}
.sidebar nav{flex:1;display:flex;flex-direction:column;gap:2px;padding:0 8px}
.sidebar nav a,.sidebar nav button{display:flex;align-items:center;gap:10px;padding:10px 14px;border-radius:8px;font-size:14px;color:#94a3b8;transition:all .15s;border:none;background:none;cursor:pointer;width:100%;text-align:left;font-family:inherit}
.sidebar nav a:hover,.sidebar nav button:hover{color:#e2e8f0;background:rgba(255,255,255,0.06)}
.sidebar nav a.active{color:#fff;background:rgba(37,99,235,0.15);border-left:3px solid #2563eb;padding-left:11px}
.sidebar .nav-bottom{margin-top:auto;padding:12px 8px 20px;border-top:1px solid rgba(255,255,255,0.08)}
.sidebar .nav-icon{width:20px;text-align:center;font-size:15px;flex-shrink:0}
.content{margin-left:240px;flex:1;min-width:0}
.topbar{height:60px;background:#fff;border-bottom:1px solid #e2e8f0;display:flex;align-items:center;justify-content:space-between;padding:0 32px;position:sticky;top:0;z-index:50}
.topbar h1{font-size:18px;font-weight:700;color:#0f172a}
.topbar .breadcrumb{display:flex;align-items:center;gap:6px;font-size:13px;color:#64748b}
.topbar .breadcrumb a{color:#3b82f6}
.topbar .breadcrumb a:hover{text-decoration:underline}
.main{padding:28px 32px 48px}
.card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 1px 3px rgba(0,0,0,0.06)}
.msg{border-radius:8px;padding:12px 16px;margin-bottom:20px;font-size:14px}
.msg.error{background:#fef2f2;color:#dc2626;border:1px solid #fecaca}
.msg.ok{background:#f0fdf4;color:#16a34a;border:1px solid #bbf7d0}
table{width:100%;border-collapse:collapse}
th{padding:10px 12px;text-align:left;font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.05em;color:#64748b;border-bottom:2px solid #e2e8f0}
td{padding:12px;border-bottom:1px solid #f1f5f9;vertical-align:top}
tbody tr:hover{background:#f8fafc}
.pill{padding:3px 10px;border-radius:999px;font-size:12px;font-weight:600;display:inline-block}
.pill.green{background:#dcfce7;color:#16a34a}
.pill.red{background:#fef2f2;color:#dc2626}
.pill.blue{background:#dbeafe;color:#2563eb}
.pill.orange{background:#fff7ed;color:#ea580c}
.pill.gray{background:#f1f5f9;color:#64748b}
.btn{border:0;border-radius:8px;padding:8px 16px;font-size:13px;font-weight:600;cursor:pointer;display:inline-flex;align-items:center;gap:6px;transition:all .15s;font-family:inherit}
.btn-primary{background:#0f609b;color:#fff}
.btn-primary:hover{background:#0d5289}
.btn-success{background:#16a34a;color:#fff}
.btn-success:hover{background:#15803d}
.btn-danger{background:#dc2626;color:#fff}
.btn-danger:hover{background:#b91c1c}
.btn-warning{background:#ea580c;color:#fff}
.btn-warning:hover{background:#c2410c}
.btn-ghost{background:#f1f5f9;color:#475569}
.btn-ghost:hover{background:#e2e8f0}
.btn-sm{padding:5px 10px;font-size:12px}
input,select{width:100%;margin-top:6px;padding:10px 12px;border-radius:8px;border:1px solid #d1d5db;font-size:14px;font-family:inherit;transition:border-color .15s}
input:focus,select:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,0.12)}
label{display:block;font-size:13px;font-weight:600;color:#374151;margin-top:12px}
.stats-row{display:flex;gap:16px;margin-bottom:24px;flex-wrap:wrap}
.stat-card{background:#fff;border-radius:12px;padding:20px;box-shadow:0 1px 3px rgba(0,0,0,0.06);flex:1;min-width:150px;border-left:4px solid #e2e8f0}
.stat-card.blue{border-left-color:#2563eb}
.stat-card.green{border-left-color:#16a34a}
.stat-card.red{border-left-color:#dc2626}
.stat-card.orange{border-left-color:#ea580c}
.stat-card.purple{border-left-color:#7c3aed}
.stat-card.cyan{border-left-color:#0891b2}
.stat-card .number{font-size:28px;font-weight:800;line-height:1.1;color:#0f172a}
.stat-card .label{font-size:13px;color:#64748b;margin-top:4px}
.grid{display:grid;grid-template-columns:360px 1fr;gap:24px;align-items:start}
.muted{color:#64748b;font-size:14px}
.mono{font-family:"Cascadia Code",Consolas,"Courier New",monospace;font-size:12px;word-break:break-all}
.form-actions{margin-top:16px}
`

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

const loginTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX - Admin Login</title>
  <style>
    *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
    body{font-family:"Inter","Segoe UI",sans-serif;background:linear-gradient(135deg,#0c1929 0%,#1a365d 50%,#1e3a5f 100%);color:#1e293b;min-height:100vh;display:grid;place-items:center}
    .login-wrapper{width:min(420px,92vw)}
    .brand{text-align:center;margin-bottom:28px;font-size:28px;font-weight:800;color:#fff;letter-spacing:-0.5px}
    .brand span{color:#3b82f6}
    .card{background:#fff;border-radius:16px;padding:36px 32px;box-shadow:0 25px 60px rgba(0,0,0,0.35)}
    .card h2{font-size:20px;font-weight:700;margin-bottom:4px}
    .card p{font-size:14px;color:#64748b;margin-bottom:24px}
    .error{background:#fef2f2;color:#dc2626;border:1px solid #fecaca;border-radius:8px;padding:10px 14px;margin-bottom:18px;font-size:14px}
    label{display:block;font-size:13px;font-weight:600;color:#374151;margin-top:16px}
    input{width:100%;margin-top:6px;padding:12px;border-radius:8px;border:1px solid #d1d5db;font-size:14px;font-family:inherit}
    input:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,0.15)}
    button{margin-top:24px;width:100%;padding:12px;border:0;border-radius:8px;background:#0f609b;color:#fff;font-weight:700;font-size:15px;cursor:pointer;transition:background .15s}
    button:hover{background:#0d5289}
  </style>
</head>
<body>
  <div class="login-wrapper">
    <div class="brand">remote<span>X</span></div>
    <div class="card">
      <h2>Painel Administrativo</h2>
      <p>Entre com suas credenciais de administrador.</p>
      {{if .Error}}<div class="error">{{.Error}}</div>{{end}}
      <form method="post" action="/admin/login">
        <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
        <label for="email">E-mail</label>
        <input id="email" name="email" type="email" autocomplete="username" placeholder="admin@empresa.com" />
        <label for="password">Senha</label>
        <input id="password" name="password" type="password" autocomplete="current-password" />
        <button type="submit">Entrar</button>
      </form>
    </div>
  </div>
</body>
</html>`

const dashboardTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Empresas</title>
  <style>` + sharedCSS + `</style>
</head>
<body>
  <div class="layout">
    <aside class="sidebar">
      <div class="logo">remote<span>X</span></div>
      <nav>
        <a href="/admin" class="active"><span class="nav-icon">&#9670;</span> Empresas</a>
        <a href="/admin/machines"><span class="nav-icon">&#9683;</span> Maquinas</a>
        <a href="/admin/releases"><span class="nav-icon">&#8682;</span> Releases Host</a>
        <a href="/admin/releases/viewer"><span class="nav-icon">&#9654;</span> Releases Viewer</a>
        <a href="/admin/releases/updater"><span class="nav-icon">&#8635;</span> Releases Updater</a>
      </nav>
      <div class="nav-bottom">
        <form method="post" action="/admin/logout">
          <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
          <button type="submit"><span class="nav-icon">&#10132;</span> Sair</button>
        </form>
      </div>
    </aside>
    <div class="content">
      <div class="topbar">
        <h1>Empresas</h1>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="stats-row">
          <div class="stat-card green"><div class="number">{{.Stats.ActiveCompanies}}</div><div class="label">Empresas Ativas</div></div>
          <div class="stat-card red"><div class="number">{{.Stats.BlockedCompanies}}</div><div class="label">Empresas Bloqueadas</div></div>
          <div class="stat-card purple"><div class="number">{{.Stats.TotalUsers}}</div><div class="label">Usuarios</div></div>
          <div class="stat-card blue"><div class="number">{{.Stats.OnlineMachines}}/{{.Stats.TotalMachines}}</div><div class="label">Maquinas Online</div></div>
          <div class="stat-card orange"><div class="number">{{.Stats.SessionsToday}}</div><div class="label">Sessoes Hoje</div></div>
          <div class="stat-card cyan"><div class="number">{{.Stats.ActiveSessions}}</div><div class="label">Conexoes Ativas</div></div>
        </div>
        {{if .Health}}
        <section class="card" style="margin-bottom:24px">
          <div style="display:flex;justify-content:space-between;align-items:flex-start;gap:16px;flex-wrap:wrap;margin-bottom:20px">
            <div>
              <h3 style="margin-bottom:6px">Saude do backend</h3>
              <div class="muted">Snapshot interno de <span class="mono">/health</span>, agora visivel apenas no painel admin.</div>
            </div>
            <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
              <span class="pill {{.Health.StatusClass}}">{{.Health.Status}}</span>
              <span class="muted">Servico: <strong style="color:#0f172a">{{.Health.Service}}</strong></span>
              <span class="muted">Atualizado: <strong style="color:#0f172a">{{.Health.CheckedAt}}</strong></span>
            </div>
          </div>
          <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:24px;align-items:start">
            <section>
              <h3 style="margin-bottom:12px">Componentes</h3>
              <table>
                <thead>
                  <tr>
                    <th>Componente</th>
                    <th>Status</th>
                    <th>Modo</th>
                    <th>Detalhe</th>
                  </tr>
                </thead>
                <tbody>
                  {{range .Health.Components}}
                  <tr>
                    <td><strong>{{.Name}}</strong><div class="muted">Checked: {{.CheckedAt}}</div></td>
                    <td><span class="pill {{.StatusClass}}">{{.Status}}</span></td>
                    <td>{{if .Mode}}<span class="mono">{{.Mode}}</span>{{else}}<span class="muted">—</span>{{end}}</td>
                    <td>
                      {{if .Detail}}<div>{{.Detail}}</div>{{else}}<div class="muted">—</div>{{end}}
                      {{if .LastError}}<div class="muted" style="margin-top:6px">Erro: {{.LastError}}</div>{{end}}
                    </td>
                  </tr>
                  {{else}}
                  <tr><td colspan="4" class="muted" style="text-align:center;padding:24px">Nenhum componente registrado.</td></tr>
                  {{end}}
                </tbody>
              </table>
            </section>
            <section>
              <h3 style="margin-bottom:12px">Monitores</h3>
              <table>
                <thead>
                  <tr>
                    <th>Monitor</th>
                    <th>Status</th>
                    <th>Intervalo</th>
                    <th>Ultima execucao</th>
                  </tr>
                </thead>
                <tbody>
                  {{range .Health.Monitors}}
                  <tr>
                    <td><strong>{{.Name}}</strong></td>
                    <td><span class="pill {{.StatusClass}}">{{.Status}}</span></td>
                    <td>{{.IntervalLabel}}</td>
                    <td>
                      <div>Run: {{.LastRunAt}}</div>
                      <div class="muted">OK: {{.LastSuccessAt}} | Falha: {{.LastFailureAt}}</div>
                      <div class="muted">Count: {{.LastCount}}</div>
                      {{if .LastError}}<div class="muted" style="margin-top:6px">Erro: {{.LastError}}</div>{{end}}
                    </td>
                  </tr>
                  {{else}}
                  <tr><td colspan="4" class="muted" style="text-align:center;padding:24px">Nenhum monitor registrado.</td></tr>
                  {{end}}
                </tbody>
              </table>
            </section>
          </div>
        </section>
        {{end}}
        <div class="grid">
          <section class="card">
            <h3 style="margin-bottom:16px">Nova empresa</h3>
            <form method="post" action="/admin/companies">
              <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
              <label for="name">Nome</label>
              <input id="name" name="name" type="text" placeholder="Nome da empresa" />
              <div class="form-actions">
                <button class="btn btn-primary" type="submit">Criar empresa</button>
              </div>
            </form>
          </section>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Todas as empresas</h3></div>
            <table>
              <thead>
                <tr>
                  <th>Empresa</th>
                  <th>Status</th>
                  <th>Usuarios</th>
                  <th>Maquinas</th>
                  <th>Online</th>
                  <th>Conexoes</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Companies}}
                <tr>
                  <td><strong>{{.Name}}</strong></td>
                  <td><span class="pill {{if .IsActive}}green{{else}}red{{end}}">{{if .IsActive}}Ativa{{else}}Bloqueada{{end}}</span></td>
                  <td>{{.UserCount}}</td>
                  <td>{{.MachineCount}}</td>
                  <td>{{.OnlineMachineCount}}</td>
                  <td>{{.ActiveSessionsLabel}}</td>
                  <td><a class="btn btn-primary btn-sm" href="/admin/companies/{{.ID}}">Abrir</a></td>
                </tr>
                {{else}}
                <tr><td colspan="7" class="muted" style="text-align:center;padding:32px">Nenhuma empresa cadastrada.</td></tr>
                {{end}}
              </tbody>
            </table>
          </section>
        </div>
      </div>
    </div>
  </div>
</body>
</html>`

const adminSetupTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX - Configurar TOTP</title>
  <style>
    *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
    body{font-family:"Inter","Segoe UI",sans-serif;background:linear-gradient(135deg,#0c1929 0%,#1a365d 50%,#1e3a5f 100%);color:#1e293b;min-height:100vh;display:grid;place-items:center}
    .login-wrapper{width:min(560px,94vw)}
    .brand{text-align:center;margin-bottom:28px;font-size:28px;font-weight:800;color:#fff;letter-spacing:-0.5px}
    .brand span{color:#3b82f6}
    .card{background:#fff;border-radius:16px;padding:36px 32px;box-shadow:0 25px 60px rgba(0,0,0,0.35)}
    .card h2{font-size:20px;font-weight:700;margin-bottom:6px}
    .card p{font-size:14px;color:#64748b;line-height:1.5}
    .error{background:#fef2f2;color:#dc2626;border:1px solid #fecaca;border-radius:8px;padding:10px 14px;margin:18px 0;font-size:14px}
    .panel{margin-top:18px;background:#f8fafc;border:1px solid #e2e8f0;border-radius:12px;padding:16px}
    .panel h3{font-size:13px;font-weight:700;color:#0f172a;margin-bottom:8px;text-transform:uppercase;letter-spacing:.04em}
    .mono{font-family:"Cascadia Code",Consolas,"Courier New",monospace;font-size:12px;word-break:break-all;color:#0f172a}
    label{display:block;font-size:13px;font-weight:600;color:#374151;margin-top:18px}
    input{width:100%;margin-top:6px;padding:12px;border-radius:8px;border:1px solid #d1d5db;font-size:14px;font-family:inherit}
    input:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,0.15)}
    button{margin-top:24px;width:100%;padding:12px;border:0;border-radius:8px;background:#0f609b;color:#fff;font-weight:700;font-size:15px;cursor:pointer;transition:background .15s}
    button:hover{background:#0d5289}
    .hint{margin-top:14px;font-size:13px;color:#64748b}
  </style>
</head>
<body>
  <div class="login-wrapper">
    <div class="brand">remote<span>X</span></div>
    <div class="card">
      <h2>ConfiguraÃ§Ã£o do TOTP administrativo</h2>
      <p>Cadastre este segredo no seu autenticador e confirme com o primeiro cÃ³digo antes de abrir a sessÃ£o administrativa.</p>
      {{if .Error}}<div class="error">{{.Error}}</div>{{end}}
      <div class="panel">
        <h3>Conta</h3>
        <div class="mono">{{.Email}}</div>
      </div>
      <div class="panel">
        <h3>Segredo TOTP</h3>
        <div class="mono">{{.Secret}}</div>
      </div>
      <div class="panel">
        <h3>URI otpauth</h3>
        <div class="mono">{{.OTPAuthURL}}</div>
      </div>
      <form method="post" action="/admin/setup">
        <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
        <label for="code">CÃ³digo do autenticador</label>
        <input id="code" name="code" type="text" inputmode="numeric" autocomplete="one-time-code" placeholder="000000" />
        <button type="submit">Confirmar TOTP e entrar</button>
      </form>
      <div class="hint">Sem um cÃ³digo vÃ¡lido o RemoteX nÃ£o grava a inscriÃ§Ã£o e nÃ£o cria a sessÃ£o final de admin.</div>
    </div>
  </div>
</body>
</html>`

const adminVerifyTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX - Verificar TOTP</title>
  <style>
    *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
    body{font-family:"Inter","Segoe UI",sans-serif;background:linear-gradient(135deg,#0c1929 0%,#1a365d 50%,#1e3a5f 100%);color:#1e293b;min-height:100vh;display:grid;place-items:center}
    .login-wrapper{width:min(420px,92vw)}
    .brand{text-align:center;margin-bottom:28px;font-size:28px;font-weight:800;color:#fff;letter-spacing:-0.5px}
    .brand span{color:#3b82f6}
    .card{background:#fff;border-radius:16px;padding:36px 32px;box-shadow:0 25px 60px rgba(0,0,0,0.35)}
    .card h2{font-size:20px;font-weight:700;margin-bottom:6px}
    .card p{font-size:14px;color:#64748b;line-height:1.5}
    .error{background:#fef2f2;color:#dc2626;border:1px solid #fecaca;border-radius:8px;padding:10px 14px;margin:18px 0;font-size:14px}
    .account{margin-top:18px;font-size:13px;color:#475569;background:#f8fafc;border:1px solid #e2e8f0;border-radius:10px;padding:12px 14px}
    label{display:block;font-size:13px;font-weight:600;color:#374151;margin-top:18px}
    input{width:100%;margin-top:6px;padding:12px;border-radius:8px;border:1px solid #d1d5db;font-size:14px;font-family:inherit}
    input:focus{outline:none;border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,0.15)}
    button{margin-top:24px;width:100%;padding:12px;border:0;border-radius:8px;background:#0f609b;color:#fff;font-weight:700;font-size:15px;cursor:pointer;transition:background .15s}
    button:hover{background:#0d5289}
  </style>
</head>
<body>
  <div class="login-wrapper">
    <div class="brand">remote<span>X</span></div>
    <div class="card">
      <h2>VerificaÃ§Ã£o do TOTP administrativo</h2>
      <p>Digite o cÃ³digo do autenticador para concluir o login do painel administrativo.</p>
      {{if .Error}}<div class="error">{{.Error}}</div>{{end}}
      <div class="account">Conta: <strong>{{.Email}}</strong></div>
      <form method="post" action="/admin/verify">
        <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
        <label for="code">CÃ³digo do autenticador</label>
        <input id="code" name="code" type="text" inputmode="numeric" autocomplete="one-time-code" placeholder="000000" />
        <button type="submit">Validar e entrar</button>
      </form>
    </div>
  </div>
</body>
</html>`
