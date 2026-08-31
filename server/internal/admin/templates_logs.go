package admin

const companyLogsTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Logs {{.Company.Name}}</title>
  <style>` + sharedCSS + `
    .filter-bar{display:flex;gap:12px;flex-wrap:wrap;align-items:end;margin-bottom:20px}
    .filter-bar .field{flex:1;min-width:140px}
    .filter-bar label{margin-top:0}
    .pagination{display:flex;align-items:center;justify-content:center;gap:8px;margin-top:20px;padding:16px 0}
    .pagination a,.pagination span{padding:6px 14px;border-radius:6px;font-size:13px;font-weight:600}
    .pagination a{background:#f1f5f9;color:#475569;text-decoration:none}
    .pagination a:hover{background:#e2e8f0}
    .pagination .current{background:#0f609b;color:#fff}
    .pagination .info{color:#64748b;font-size:13px}
    .event-type{font-size:12px;font-weight:600;padding:3px 8px;border-radius:4px;display:inline-block}
    .event-type.session{background:#dbeafe;color:#2563eb}
    .event-type.transfer{background:#fef3c7;color:#d97706}
    .event-type.error{background:#fef2f2;color:#dc2626}
    .event-type.other{background:#f1f5f9;color:#64748b}
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
          <a href="/admin">Empresas</a> <span>/</span>
          <a href="/admin/companies/{{.Company.ID}}">{{.Company.Name}}</a> <span>/</span>
          <strong style="color:#0f172a">Logs de Auditoria</strong>
        </div>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        <div class="card">
          <form method="get" action="/admin/companies/{{.Company.ID}}/logs">
            <div class="filter-bar">
              <div class="field">
                <label for="event_type">Tipo de evento</label>
                <select id="event_type" name="event_type">
                  <option value="">Todos</option>
                  <option value="session_started" {{if eq .Filter.EventType "session_started"}}selected{{end}}>Sessao iniciada</option>
                  <option value="session_ended" {{if eq .Filter.EventType "session_ended"}}selected{{end}}>Sessao encerrada</option>
                  <option value="file_upload_started" {{if eq .Filter.EventType "file_upload_started"}}selected{{end}}>Upload iniciado</option>
                  <option value="file_upload_finished" {{if eq .Filter.EventType "file_upload_finished"}}selected{{end}}>Upload concluido</option>
                  <option value="file_download_started" {{if eq .Filter.EventType "file_download_started"}}selected{{end}}>Download iniciado</option>
                  <option value="file_download_finished" {{if eq .Filter.EventType "file_download_finished"}}selected{{end}}>Download concluido</option>
                  <option value="file_transfer_failed" {{if eq .Filter.EventType "file_transfer_failed"}}selected{{end}}>Transferencia falhou</option>
                </select>
              </div>
              <div class="field">
                <label for="date_from">Data inicio</label>
                <input id="date_from" name="date_from" type="date" value="{{.Filter.DateFrom}}" />
              </div>
              <div class="field">
                <label for="date_to">Data fim</label>
                <input id="date_to" name="date_to" type="date" value="{{.Filter.DateTo}}" />
              </div>
              <div class="field">
                <label for="search">Busca</label>
                <input id="search" name="search" type="text" value="{{.Filter.Search}}" placeholder="Buscar no resumo..." />
              </div>
              <div class="field" style="flex:0">
                <button class="btn btn-primary" type="submit" style="margin-top:6px;white-space:nowrap">Filtrar</button>
              </div>
            </div>
          </form>
          <div class="muted" style="margin-bottom:12px">{{.Total}} evento(s) encontrado(s)</div>
          <div style="overflow-x:auto">
            <table>
              <thead>
                <tr>
                  <th>Data/Hora</th>
                  <th>Tipo</th>
                  <th>Maquina</th>
                  <th>Usuario</th>
                  <th>Resumo</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                {{range .Events}}
                <tr>
                  <td class="muted" style="white-space:nowrap">{{.CreatedAt.Format "02/01/2006 15:04:05"}}</td>
                  <td>
                    <span class="event-type {{if or (eq .EventType "session_started") (eq .EventType "session_ended")}}session{{else if or (eq .EventType "file_transfer_failed")}}error{{else if or (eq .EventType "file_upload_started") (eq .EventType "file_upload_finished") (eq .EventType "file_download_started") (eq .EventType "file_download_finished")}}transfer{{else}}other{{end}}">{{.EventType}}</span>
                  </td>
                  <td>{{if .MachineName}}{{.MachineName}}{{else}}<span class="muted">-</span>{{end}}</td>
                  <td>{{if .UserName}}{{.UserName}}{{else}}<span class="muted">-</span>{{end}}</td>
                  <td>{{.Summary}}</td>
                  <td>{{if .Status}}<span class="pill gray">{{.Status}}</span>{{else}}<span class="muted">-</span>{{end}}</td>
                </tr>
                {{else}}
                <tr><td colspan="6" class="muted" style="text-align:center;padding:32px">Nenhum evento encontrado.</td></tr>
                {{end}}
              </tbody>
            </table>
          </div>
          {{if gt .TotalPages 1}}
          <div class="pagination">
            {{if gt .Page 1}}
            <a href="/admin/companies/{{.Company.ID}}/logs?page={{.PrevPage}}&event_type={{urlquery .Filter.EventType}}&machine_id={{urlquery .Filter.MachineID}}&date_from={{urlquery .Filter.DateFrom}}&date_to={{urlquery .Filter.DateTo}}&search={{urlquery .Filter.Search}}">Anterior</a>
            {{end}}
            <span class="info">Pagina {{.Page}} de {{.TotalPages}}</span>
            {{if lt .Page .TotalPages}}
            <a href="/admin/companies/{{.Company.ID}}/logs?page={{.NextPage}}&event_type={{urlquery .Filter.EventType}}&machine_id={{urlquery .Filter.MachineID}}&date_from={{urlquery .Filter.DateFrom}}&date_to={{urlquery .Filter.DateTo}}&search={{urlquery .Filter.Search}}">Proxima</a>
            {{end}}
          </div>
          {{end}}
        </div>
      </div>
    </div>
  </div>
</body>
</html>`
