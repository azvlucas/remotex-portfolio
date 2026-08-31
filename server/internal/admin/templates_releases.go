package admin

const releasesTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Releases Host</title>
  <style>` + sharedCSS + `
    input[type="file"]{padding:10px;background:#f8fafc}
  </style>
</head>
<body>
  <div class="layout">
    <aside class="sidebar">
      <div class="logo">remote<span>X</span></div>
      <nav>
        <a href="/admin"><span class="nav-icon">&#9670;</span> Empresas</a>
        <a href="/admin/machines"><span class="nav-icon">&#9683;</span> Maquinas</a>
        <a href="/admin/releases" class="active"><span class="nav-icon">&#8682;</span> Releases Host</a>
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
        <h1>Releases do Host</h1>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="grid">
          <div>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:12px">Publicar release</h3>
              <p class="muted" style="margin-bottom:12px">Envie o arquivo .zip gerado pelo build_release.bat. O pacote precisa conter release.json e o binario assinado.</p>
              <form method="post" action="/admin/releases/upload" enctype="multipart/form-data">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <label for="package">Pacote da release</label>
                <input id="package" name="package" type="file" accept=".zip,application/zip" />
                <div class="form-actions">
                  <button class="btn btn-primary" type="submit">Enviar pacote</button>
                </div>
              </form>
            </section>
            <section class="card">
              <h3 style="margin-bottom:12px">Release efetiva</h3>
              {{if .Current}}
              <div style="font-size:14px;line-height:2">
                <div><strong>Versao:</strong> {{.Current.Version}}</div>
                <div><strong>Binario:</strong> {{.Current.Binary}}</div>
                <div><strong>Tamanho:</strong> {{.Current.FileSize}} bytes</div>
                <div><strong>Origem:</strong> <span class="pill {{if eq .Current.Source "published"}}green{{else}}blue{{end}}">{{if eq .Current.Source "published"}}Publicada{{else}}Embutida{{end}}</span></div>
              </div>
              {{else}}
              <div class="muted">Nenhuma release disponivel no momento.</div>
              {{end}}
            </section>
          </div>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Releases publicadas</h3></div>
            <table>
              <thead>
                <tr>
                  <th>Versao</th>
                  <th>Status</th>
                  <th>Tamanho</th>
                  <th>Publicada em</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Releases}}
                <tr>
                  <td>
                    <strong>{{.Version}}</strong>
                    <div class="muted">{{.Binary}}</div>
                  </td>
                  <td><span class="pill {{if .Active}}green{{else}}orange{{end}}">{{if .Active}}Ativa{{else}}Inativa{{end}}</span></td>
                  <td class="muted">{{.FileSize}} bytes</td>
                  <td class="muted">{{.UploadedAt.Format "2006-01-02 15:04:05 UTC"}}</td>
                  <td>
                    <div style="display:flex;gap:4px">
                      {{if .Active}}
                      <button class="btn btn-ghost btn-sm" disabled>Em uso</button>
                      {{else}}
                      <form method="post" action="/admin/releases/{{.Version}}/activate">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-primary btn-sm" type="submit">Ativar</button>
                      </form>
                      <form method="post" action="/admin/releases/{{.Version}}/delete" onsubmit="return confirm('Excluir release {{.Version}}?')">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-danger btn-sm" type="submit">Excluir</button>
                      </form>
                      {{end}}
                    </div>
                  </td>
                </tr>
                {{else}}
                <tr><td colspan="5" class="muted" style="text-align:center;padding:32px">Nenhuma release publicada pelo painel.</td></tr>
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

const viewerReleasesTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Releases Viewer</title>
  <style>` + sharedCSS + `
    input[type="file"]{padding:10px;background:#f8fafc}
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
        <a href="/admin/releases/viewer" class="active"><span class="nav-icon">&#9654;</span> Releases Viewer</a>
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
        <h1>Releases do Viewer</h1>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="grid">
          <div>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:12px">Publicar release</h3>
              <p class="muted" style="margin-bottom:12px">Envie o arquivo .zip do viewer. O pacote precisa conter release.json e o binario.</p>
              <form method="post" action="/admin/releases/viewer/upload" enctype="multipart/form-data">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <label for="package">Pacote da release</label>
                <input id="package" name="package" type="file" accept=".zip,application/zip" />
                <div class="form-actions">
                  <button class="btn btn-primary" type="submit">Enviar pacote</button>
                </div>
              </form>
            </section>
            <section class="card">
              <h3 style="margin-bottom:12px">Release efetiva</h3>
              {{if .Current}}
              <div style="font-size:14px;line-height:2">
                <div><strong>Versao:</strong> {{.Current.Version}}</div>
                <div><strong>Binario:</strong> {{.Current.Binary}}</div>
                <div><strong>Tamanho:</strong> {{.Current.FileSize}} bytes</div>
                <div><strong>Origem:</strong> <span class="pill {{if eq .Current.Source "published"}}green{{else}}blue{{end}}">{{if eq .Current.Source "published"}}Publicada{{else}}Embutida{{end}}</span></div>
              </div>
              {{else}}
              <div class="muted">Nenhuma release disponivel no momento.</div>
              {{end}}
            </section>
          </div>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Releases publicadas</h3></div>
            <table>
              <thead>
                <tr>
                  <th>Versao</th>
                  <th>Status</th>
                  <th>Tamanho</th>
                  <th>Publicada em</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Releases}}
                <tr>
                  <td>
                    <strong>{{.Version}}</strong>
                    <div class="muted">{{.Binary}}</div>
                  </td>
                  <td><span class="pill {{if .Active}}green{{else}}orange{{end}}">{{if .Active}}Ativa{{else}}Inativa{{end}}</span></td>
                  <td class="muted">{{.FileSize}} bytes</td>
                  <td class="muted">{{.UploadedAt.Format "2006-01-02 15:04:05 UTC"}}</td>
                  <td>
                    <div style="display:flex;gap:4px">
                      {{if .Active}}
                      <button class="btn btn-ghost btn-sm" disabled>Em uso</button>
                      {{else}}
                      <form method="post" action="/admin/releases/viewer/{{.Version}}/activate">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-primary btn-sm" type="submit">Ativar</button>
                      </form>
                      <form method="post" action="/admin/releases/viewer/{{.Version}}/delete" onsubmit="return confirm('Excluir release {{.Version}}?')">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-danger btn-sm" type="submit">Excluir</button>
                      </form>
                      {{end}}
                    </div>
                  </td>
                </tr>
                {{else}}
                <tr><td colspan="5" class="muted" style="text-align:center;padding:32px">Nenhuma release publicada pelo painel.</td></tr>
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

const updaterReleasesTemplate = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>remoteX Admin - Releases Updater</title>
  <style>` + sharedCSS + `
    input[type="file"]{padding:10px;background:#f8fafc}
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
        <a href="/admin/releases/updater" class="active"><span class="nav-icon">&#8635;</span> Releases Updater</a>
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
        <h1>Releases do Updater</h1>
      </div>
      <div class="main">
        {{if .Error}}<div class="msg error">{{.Error}}</div>{{end}}
        {{if .Message}}<div class="msg ok">{{.Message}}</div>{{end}}
        <div class="grid">
          <div>
            <section class="card" style="margin-bottom:20px">
              <h3 style="margin-bottom:12px">Publicar release</h3>
              <p class="muted" style="margin-bottom:12px">Envie o arquivo .zip do updater. O pacote precisa conter release.json e o binario.</p>
              <form method="post" action="/admin/releases/updater/upload" enctype="multipart/form-data">
                <input type="hidden" name="csrf_token" value="{{.CSRFToken}}" />
                <label for="package">Pacote da release</label>
                <input id="package" name="package" type="file" accept=".zip,application/zip" />
                <div class="form-actions">
                  <button class="btn btn-primary" type="submit">Enviar pacote</button>
                </div>
              </form>
            </section>
            <section class="card">
              <h3 style="margin-bottom:12px">Release efetiva</h3>
              {{if .Current}}
              <div style="font-size:14px;line-height:2">
                <div><strong>Versao:</strong> {{.Current.Version}}</div>
                <div><strong>Binario:</strong> {{.Current.Binary}}</div>
                <div><strong>Tamanho:</strong> {{.Current.FileSize}} bytes</div>
                <div><strong>Origem:</strong> <span class="pill {{if eq .Current.Source "published"}}green{{else}}blue{{end}}">{{if eq .Current.Source "published"}}Publicada{{else}}Embutida{{end}}</span></div>
              </div>
              {{else}}
              <div class="muted">Nenhuma release disponivel no momento.</div>
              {{end}}
            </section>
          </div>
          <section class="card" style="padding:0;overflow:hidden">
            <div style="padding:20px 24px 0"><h3>Releases publicadas</h3></div>
            <table>
              <thead>
                <tr>
                  <th>Versao</th>
                  <th>Status</th>
                  <th>Tamanho</th>
                  <th>Publicada em</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {{range .Releases}}
                <tr>
                  <td>
                    <strong>{{.Version}}</strong>
                    <div class="muted">{{.Binary}}</div>
                  </td>
                  <td><span class="pill {{if .Active}}green{{else}}orange{{end}}">{{if .Active}}Ativa{{else}}Inativa{{end}}</span></td>
                  <td class="muted">{{.FileSize}} bytes</td>
                  <td class="muted">{{.UploadedAt.Format "2006-01-02 15:04:05 UTC"}}</td>
                  <td>
                    <div style="display:flex;gap:4px">
                      {{if .Active}}
                      <button class="btn btn-ghost btn-sm" disabled>Em uso</button>
                      {{else}}
                      <form method="post" action="/admin/releases/updater/{{.Version}}/activate">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-primary btn-sm" type="submit">Ativar</button>
                      </form>
                      <form method="post" action="/admin/releases/updater/{{.Version}}/delete" onsubmit="return confirm('Excluir release {{.Version}}?')">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}" />
                        <button class="btn btn-danger btn-sm" type="submit">Excluir</button>
                      </form>
                      {{end}}
                    </div>
                  </td>
                </tr>
                {{else}}
                <tr><td colspan="5" class="muted" style="text-align:center;padding:32px">Nenhuma release publicada pelo painel.</td></tr>
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
