package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/azvlucas/remotex-portfolio/server/internal/auth"
)

const defaultTTL = 72 * time.Hour

type outputMode string

const (
	outputRaw  outputMode = "raw"
	outputJSON outputMode = "json"
)

func (m *outputMode) String() string {
	if m == nil || *m == "" {
		return string(outputRaw)
	}
	return string(*m)
}

func (m *outputMode) Set(value string) error {
	switch normalized := strings.ToLower(strings.TrimSpace(value)); normalized {
	case "", string(outputRaw):
		*m = outputRaw
		return nil
	case string(outputJSON):
		*m = outputJSON
		return nil
	default:
		return fmt.Errorf("invalid output %q (want raw or json)", value)
	}
}

type subjectType string

const subjectUser subjectType = "user"

func (s *subjectType) String() string {
	if s == nil || *s == "" {
		return string(subjectUser)
	}
	return string(*s)
}

func (s *subjectType) Set(value string) error {
	normalized := subjectType(strings.ToLower(strings.TrimSpace(value)))
	if normalized == "" {
		normalized = subjectUser
	}
	if normalized != subjectUser {
		return fmt.Errorf("unsupported subject_type %q (only user is supported)", value)
	}
	*s = normalized
	return nil
}

type cliOptions struct {
	companyID   string
	userID      string
	email       string
	name        string
	role        auth.Role
	subjectType subjectType
	ttl         time.Duration
	output      outputMode
}

type jsonOutput struct {
	Token       string           `json:"token"`
	Issuer      string           `json:"issuer"`
	Scope       string           `json:"scope"`
	CompanyID   string           `json:"company_id"`
	UserID      string           `json:"user_id"`
	Email       string           `json:"email,omitempty"`
	Name        string           `json:"name,omitempty"`
	Role        auth.Role        `json:"role"`
	SubjectType auth.SubjectType `json:"subject_type"`
	TTLSeconds  int64            `json:"ttl_seconds"`
}

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr, os.LookupEnv))
}

func run(args []string, stdout, stderr io.Writer, lookupEnv func(string) (string, bool)) int {
	opts, err := parseArgs(args)
	if err != nil {
		fmt.Fprintf(stderr, "error: %v\n\n", err)
		writeUsage(stderr)
		return 2
	}

	jwtSecret, ok := lookupEnv("JWT_SECRET")
	if !ok || strings.TrimSpace(jwtSecret) == "" {
		fmt.Fprintln(stderr, "error: JWT_SECRET is required")
		return 1
	}

	authSvc := auth.NewService(auth.DefaultConfig(strings.TrimSpace(jwtSecret)))
	token, err := authSvc.GenerateEnrollmentToken(auth.TokenSubject{
		UserID:      opts.userID,
		Email:       opts.email,
		Name:        opts.name,
		Role:        opts.role,
		CompanyID:   opts.companyID,
		SubjectType: auth.SubjectTypeUser,
	}, opts.ttl)
	if err != nil {
		fmt.Fprintf(stderr, "error: failed to mint enrollment token: %v\n", err)
		return 1
	}

	if opts.output == outputJSON {
		payload := jsonOutput{
			Token:       token,
			Issuer:      "remotex-enroll",
			Scope:       auth.ScopeHostEnroll,
			CompanyID:   opts.companyID,
			UserID:      opts.userID,
			Email:       opts.email,
			Name:        opts.name,
			Role:        opts.role,
			SubjectType: auth.SubjectTypeUser,
			TTLSeconds:  int64(opts.ttl.Seconds()),
		}
		encoder := json.NewEncoder(stdout)
		encoder.SetIndent("", "  ")
		if err := encoder.Encode(payload); err != nil {
			fmt.Fprintf(stderr, "error: failed to write json output: %v\n", err)
			return 1
		}
		return 0
	}

	if _, err := fmt.Fprintln(stdout, token); err != nil {
		fmt.Fprintf(stderr, "error: failed to write token: %v\n", err)
		return 1
	}
	return 0
}

func parseArgs(args []string) (cliOptions, error) {
	opts := cliOptions{
		role:        auth.RoleCompanyManager,
		subjectType: subjectUser,
		ttl:         defaultTTL,
		output:      outputRaw,
	}

	flagSet := flag.NewFlagSet("mint-enrollment-token", flag.ContinueOnError)
	flagSet.SetOutput(io.Discard)

	var ttlRaw string
	var roleRaw string
	flagSet.StringVar(&opts.companyID, "company-id", "", "Company ID to scope the enrollment token")
	flagSet.StringVar(&opts.userID, "user-id", "", "Human user ID that owns the enrollment token")
	flagSet.StringVar(&opts.email, "email", "", "Optional human email claim")
	flagSet.StringVar(&opts.name, "name", "", "Optional human name claim")
	flagSet.StringVar(&roleRaw, "role", string(auth.RoleCompanyManager), "Human role claim: company_manager or company_user")
	flagSet.StringVar(&ttlRaw, "ttl", defaultTTL.String(), "Token TTL duration, e.g. 72h or 30m")
	flagSet.Var(&opts.output, "output", "Output mode: raw or json")
	flagSet.Var(&opts.subjectType, "subject-type", "Subject type (only user is supported)")

	if err := flagSet.Parse(args); err != nil {
		return cliOptions{}, err
	}
	if flagSet.NArg() != 0 {
		return cliOptions{}, fmt.Errorf("unexpected positional arguments: %s", strings.Join(flagSet.Args(), " "))
	}

	opts.companyID = strings.TrimSpace(opts.companyID)
	opts.userID = strings.TrimSpace(opts.userID)
	opts.email = strings.TrimSpace(strings.ToLower(opts.email))
	opts.name = strings.TrimSpace(opts.name)
	roleRaw = strings.ToLower(strings.TrimSpace(roleRaw))

	switch auth.Role(roleRaw) {
	case auth.RoleCompanyManager, auth.RoleCompanyUser:
		opts.role = auth.Role(roleRaw)
	default:
		return cliOptions{}, fmt.Errorf("invalid role %q (want company_manager or company_user)", roleRaw)
	}

	ttl, err := time.ParseDuration(strings.TrimSpace(ttlRaw))
	if err != nil || ttl <= 0 {
		return cliOptions{}, fmt.Errorf("ttl must be a positive duration")
	}
	opts.ttl = ttl

	if opts.subjectType != subjectUser {
		return cliOptions{}, fmt.Errorf("unsupported subject_type %q (only user is supported)", opts.subjectType)
	}
	if opts.companyID == "" {
		return cliOptions{}, fmt.Errorf("company-id is required")
	}
	if opts.userID == "" {
		return cliOptions{}, fmt.Errorf("user-id is required")
	}

	return opts, nil
}

func writeUsage(w io.Writer) {
	fmt.Fprintln(w, "Usage: mint-enrollment-token -company-id <company-id> -user-id <user-id> [options]")
	fmt.Fprintln(w, "")
	fmt.Fprintln(w, "Options:")
	fmt.Fprintln(w, "  -email <email>            Optional email claim")
	fmt.Fprintln(w, "  -name <name>              Optional name claim")
	fmt.Fprintln(w, "  -role <role>              company_manager (default) or company_user")
	fmt.Fprintln(w, "  -ttl <duration>           Token TTL, default 72h")
	fmt.Fprintln(w, "  -output <raw|json>        Output format, default raw")
	fmt.Fprintln(w, "  -subject-type <user>      Reserved for scripts; only user is supported")
	fmt.Fprintln(w, "")
	fmt.Fprintln(w, "Environment:")
	fmt.Fprintln(w, "  JWT_SECRET                Required signing secret")
}
