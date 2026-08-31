package reports

import (
	"context"
	"fmt"
	"time"
)

// Service encapsula a logica de relatorios.
type Service struct {
	repo *Repository
}

// NewService cria um novo service.
func NewService(repo *Repository) *Service {
	return &Service{repo: repo}
}

// DefaultTimezone e o fuso padrao usado quando nenhum e informado.
const DefaultTimezone = "America/Sao_Paulo"

// GetDashboard retorna estatisticas agregadas com validacao de datas.
func (s *Service) GetDashboard(ctx context.Context, companyID, dateFrom, dateTo, tz string) (*DashboardData, error) {
	now := time.Now()

	if dateFrom == "" {
		dateFrom = now.AddDate(0, 0, -30).Format("2006-01-02")
	}
	if dateTo == "" {
		dateTo = now.Format("2006-01-02")
	}
	if tz == "" {
		tz = DefaultTimezone
	}

	if _, err := time.Parse("2006-01-02", dateFrom); err != nil {
		return nil, fmt.Errorf("invalid date_from format: %w", err)
	}
	if _, err := time.Parse("2006-01-02", dateTo); err != nil {
		return nil, fmt.Errorf("invalid date_to format: %w", err)
	}

	data, err := s.repo.GetDashboard(ctx, companyID, dateFrom, dateTo, tz)
	if err != nil {
		return nil, err
	}

	if data.ByMachine == nil {
		data.ByMachine = []MachineStats{}
	}
	if data.ByTechnician == nil {
		data.ByTechnician = []TechnicianStats{}
	}
	if data.Daily == nil {
		data.Daily = []DailyStats{}
	}

	return data, nil
}
