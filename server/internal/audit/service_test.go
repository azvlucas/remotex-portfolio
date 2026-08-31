package audit

import "testing"

func TestSessionAuditSummary(t *testing.T) {
	t.Parallel()

	duration := 42

	tests := []struct {
		name       string
		prefix     string
		accessMode string
		duration   *int
		want       string
	}{
		{
			name:       "interactive start",
			prefix:     "Acesso remoto iniciado",
			accessMode: "interactive",
			want:       "Acesso remoto iniciado (controle remoto)",
		},
		{
			name:       "watch only start",
			prefix:     "Acesso remoto iniciado",
			accessMode: "watch_only",
			want:       "Acesso remoto iniciado (somente visualizacao)",
		},
		{
			name:       "watch only end with duration",
			prefix:     "Acesso remoto encerrado",
			accessMode: "watch_only",
			duration:   &duration,
			want:       "Acesso remoto encerrado (somente visualizacao, 42s)",
		},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := sessionAuditSummary(tc.prefix, tc.accessMode, tc.duration)
			if got != tc.want {
				t.Fatalf("sessionAuditSummary() = %q, want %q", got, tc.want)
			}
		})
	}
}
