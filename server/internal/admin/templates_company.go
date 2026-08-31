package admin

const companyDetailTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - {{.Detail.Company.Name}}</title>
  <style>` + sharedCSS + `
    .info-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:16px}
    .info-box{background:#f8fafc;border-radius:10px;padding:14px;text-align:center}
    .info-box .val{font-size:22px;font-weight:700;color:#0f172a}
    .info-box .lbl{font-size:12px;color:#64748b;margin-top:2px}
  </style>
</head>
<body>
  <div class="layout">
    <aside class="sidebar">
      <div class="logo">remote<span>X</span></div>
      <nav>
        <a href="/admin"><span class="nav-icon">&#9670;</span> Empresas</a>
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
        <div class="breadcrumb">
          <a href="/admin">Empresas</a> <span>/</span> <strong style="color:#0f172a">{{.Detail.Company.Name}}</strong>
        </div>
        <div style="display:flex;gap:8px">
          <a class="btn btn-ghost btn-sm" href="/admin/companies/{{.Detail.Company.ID}}/logs">Logs de auditoria</a>
          {{if .Detail.Company.IsActive}}
          <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/deactivate"><input type="hidden" name="csrf_token" value="{{.CSRFToken}}" /><button class="btn btn-danger btn-sm" type="submit">Bloquear empresa</button></form>
          {{else}}
          <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/activate"><input type="hidden" name="csrf_token" value="{{.CSRFToken}}" /><button class="btn btn-success btn-sm" type="submit">Liberar empresa</button></form>
          {{end}}
        </div>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="grid">
          <div>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:12px">Resumo</h3>
              <div>Status: <span class="pill {{if .Detail.Company.IsActive}}green{{else}}red{{end}}">{{if .Detail.Company.IsActive}}Ativa{{else}}Bloqueada{{end}}</span></div>
              <div class="info-grid" style="grid-template-columns:repeat(4,1fr)">
                <div class="info-box"><div class="val">{{.Detail.Company.UserCount}}</div><div class="lbl">Usuarios</div></div>
                <div class="info-box"><div class="val">{{.Detail.Company.MachineCount}}</div><div class="lbl">Maquinas</div></div>
                <div class="info-box"><div class="val">{{.Detail.Company.OnlineMachineCount}}</div><div class="lbl">Online</div></div>
                <div class="info-box"><div class="val">{{.Detail.Company.ActiveSessionsLabel}}</div><div class="lbl">Conexoes</div></div>
              </div>
            </section>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:8px">Bloqueio automatico</h3>
              <div class="muted" style="margin-bottom:10px">
                {{if .Detail.Company.BlockAfterDays}}Expira em: <strong>{{.Detail.Company.ExpiryDate}}</strong>{{else}}Sem expiracao{{end}}
              </div>
              <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/block-days" style="display:flex;gap:8px;align-items:end">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <div style="flex:1">
                  <label for="block_after_days">Dias apos criacao</label>
                  <input id="block_after_days" name="block_after_days" type="number" min="0" placeholder="vazio = sem expiracao" value="{{if .Detail.Company.BlockAfterDays}}{{.Detail.Company.BlockAfterDaysValue}}{{end}}" />
                </div>
                <button class="btn btn-primary" type="submit" style="height:42px">Salvar</button>
              </form>
            </section>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:8px">Limite de maquinas</h3>
              <div class="muted" style="margin-bottom:10px">
                {{if .Detail.Company.MachineLimit}}Maximo: <strong>{{.Detail.Company.MachineLimitValue}}</strong> maquinas{{else}}Sem limite{{end}}
              </div>
              <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/machine-limit" style="display:flex;gap:8px;align-items:end">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <div style="flex:1">
                  <label for="machine_limit">Maquinas registradas</label>
                  <input id="machine_limit" name="machine_limit" type="number" min="0" placeholder="vazio ou 0 = sem limite" value="{{if .Detail.Company.MachineLimit}}{{.Detail.Company.MachineLimitValue}}{{end}}" />
                </div>
                <button class="btn btn-primary" type="submit" style="height:42px">Salvar</button>
              </form>
            </section>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:8px">Limite de sessoes simultaneas</h3>
              <div class="muted" style="margin-bottom:10px">
                {{if .Detail.Company.ConcurrentSessionLimit}}Maximo: <strong>{{.Detail.Company.ConcurrentSessionLimitValue}}</strong> sessoes{{else}}Sem limite{{end}}
              </div>
              <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/concurrent-limit" style="display:flex;gap:8px;align-items:end">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <div style="flex:1">
                  <label for="concurrent_session_limit">Sessoes simultaneas</label>
                  <input id="concurrent_session_limit" name="concurrent_session_limit" type="number" min="0" placeholder="vazio ou 0 = sem limite" value="{{if .Detail.Company.ConcurrentSessionLimit}}{{.Detail.Company.ConcurrentSessionLimitValue}}{{end}}" />
                </div>
                <button class="btn btn-primary" type="submit" style="height:42px">Salvar</button>
              </form>
            </section>
            <section class="card">
              <h3 style="margin-bottom:12px">Adicionar usuario</h3>
              <form method="post" action="/admin/companies/{{.Detail.Company.ID}}/users">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <label for="uname">Nome</label>
                <input id="uname" name="name" type="text" />
                <label for="uemail">E-mail</label>
                <input id="uemail" name="email" type="email" />
                <label for="upassword">Senha</label>
                <input id="upassword" name="password" type="password" />
                <label for="urole">Cargo</label>
                <select id="urole" name="role">
                  <option value="company_user">Usuario</option>
                  <option value="company_manager">Gerente</option>
                </select>
                <div class="form-actions">
                  <button class="btn btn-primary" type="submit">Criar usuario</button>
                </div>
              </form>
            </section>
          </div>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Usuarios</h3></div>
            <table>
              <thead>
                <tr>
                  <th>Nome</th>
                  <th>E-mail</th>
                  <th>Cargo</th>
                  <th>Status</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Detail.Users}}
                <tr>
                  <td><strong>{{.Name}}</strong></td>
                  <td>{{.Email}}</td>
                  <td><span class="pill blue">{{if eq (printf "%s" .Role) "company_manager"}}Gerente{{else}}Usuario{{end}}</span></td>
                  <td><span class="pill {{if .IsActive}}green{{else}}red{{end}}">{{if .IsActive}}Ativo{{else}}Bloqueado{{end}}</span></td>
                  <td>
                    <div style="display:flex;gap:4px;flex-wrap:wrap">
                      {{if eq (printf "%s" .Role) "company_manager"}}
                      <form method="post" action="/admin/users/{{.ID}}/role">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <input type="hidden" name="company_id" value="{{$.Detail.Company.ID}}" />
                        <input type="hidden" name="role" value="company_user" />
                        <button class="btn btn-warning btn-sm" type="submit">Rebaixar</button>
                      </form>
                      {{else}}
                      <form method="post" action="/admin/users/{{.ID}}/role">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <input type="hidden" name="company_id" value="{{$.Detail.Company.ID}}" />
                        <input type="hidden" name="role" value="company_manager" />
                        <button class="btn btn-success btn-sm" type="submit">Promover</button>
                      </form>
                      {{end}}
                      {{if .IsActive}}
                      <form method="post" action="/admin/users/{{.ID}}/deactivate">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <input type="hidden" name="company_id" value="{{$.Detail.Company.ID}}" />
                        <button class="btn btn-danger btn-sm" type="submit">Bloquear</button>
                      </form>
                      {{else}}
                      <form method="post" action="/admin/users/{{.ID}}/activate">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <input type="hidden" name="company_id" value="{{$.Detail.Company.ID}}" />
                        <button class="btn btn-primary btn-sm" type="submit">Liberar</button>
                      </form>
                      {{end}}
                    </div>
                  </td>
                </tr>
                {{else}}
                <tr><td colspan="5" class="muted" style="text-align:center;padding:32px">Nenhum usuario nesta empresa.</td></tr>
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

const machinesTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Maquinas</title>
  <style>` + sharedCSS + `</style>
</head>
<body>
  <div class="layout">
    <aside class="sidebar">
      <div class="logo">remote<span>X</span></div>
      <nav>
        <a href="/admin"><span class="nav-icon">&#9670;</span> Empresas</a>
        <a href="/admin/machines" class="active"><span class="nav-icon">&#9683;</span> Maquinas</a>
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
        <h1>Busca Global de Maquinas</h1>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="grid">
          <section class="card">
            <h3 style="margin-bottom:12px">Pesquisar</h3>
            <p class="muted" style="margin-bottom:12px">Busque por nome da maquina, ID, machine key ou nome da empresa.</p>
            <form method="get" action="/admin/machines">
              <label for="q">Consulta</label>
              <input id="q" name="q" type="text" value="{{.Query}}" placeholder="Ex.: host-cliente, UUID, machine key" />
              <div class="form-actions">
                <button class="btn btn-primary" type="submit">Pesquisar</button>
              </div>
            </form>
          </section>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Resultados</h3></div>
            {{if not .Query}}
            <div class="muted" style="padding:24px;text-align:center">Informe uma busca para localizar uma maquina especifica.</div>
            {{else}}
            <table>
              <thead>
                <tr>
                  <th>Maquina</th>
                  <th>Empresa</th>
                  <th>Status</th>
                  <th>Heartbeat</th>
                  <th>Atualizada</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Machines}}
                <tr>
                  <td>
                    <strong>{{.Name}}</strong>
                    <div class="mono" style="margin-top:4px">ID: {{.ID}}</div>
                    <div class="mono">Key: {{.MachineKey}}</div>
                  </td>
                  <td>{{.CompanyName}}</td>
                  <td>
                    <span class="pill {{.StatusPillClass}}">{{.StatusLabel}}</span>
                    {{if .IsBlocked}}
                    <div class="muted" style="margin-top:4px">{{.BlockReasonLabel}}</div>
                    <div style="margin-top:6px"><span class="pill {{.ConnectivityPillClass}}">{{.ConnectivityLabel}}</span></div>
                    {{end}}
                  </td>
                  <td class="muted">{{.LastHeartbeatLabel}}</td>
                  <td class="muted">{{.UpdatedAtLabel}}</td>
                  <td>
                    {{if .CanDetach}}
                    <form method="post" action="/admin/machines/{{.ID}}/detach" onsubmit="return confirm('Desvincular a maquina {{.Name}} da empresa {{.CompanyName}}?');">
                      <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                      <input type="hidden" name="q" value="{{$.Query}}" />
                      <button class="btn btn-warning btn-sm" type="submit">Desvincular</button>
                    </form>
                    {{else}}
                    <span class="muted">Ja desvinculada</span>
                    {{end}}
                  </td>
                </tr>
                {{else}}
                <tr><td colspan="6" class="muted" style="text-align:center;padding:32px">Nenhuma maquina encontrada para a busca informada.</td></tr>
                {{end}}
              </tbody>
            </table>
            {{end}}
          </section>
        </div>
      </div>
    </div>
  </div>
</body>
</html>`
