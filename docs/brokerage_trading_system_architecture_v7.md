# V7 Full-Scale Brokerage Trading System Architecture

## 0. Document Purpose

This document upgrades the previous **V6 Isolated Trading System Architecture** into a more complete **production-oriented brokerage system architecture**.

The V6 design focused mainly on isolated matching engines, deterministic execution, event sourcing, low-latency order processing, and fault-domain separation.

The V7 design expands the system into a broader brokerage platform that can support:

- client onboarding
- KYC / KYB
- account management
- funding and withdrawals
- order management
- pre-trade risk
- market data
- matching or external execution
- positions
- margin
- clearing
- settlement
- custody
- ledger accounting
- compliance
- audit
- reporting
- operations
- administration
- monitoring
- disaster recovery

The goal is not only to build a high-performance trading core, but also to design a system that can gradually evolve into a usable, functionally complete brokerage platform.

---

# 1. Important Architecture Clarification

A brokerage system is not the same as an exchange system.

## 1.1 Exchange System

An exchange usually provides:

- central limit order book
- matching engine
- market data dissemination
- trade execution venue
- clearing integration
- participant access

## 1.2 Brokerage System

A broker usually provides:

- client onboarding
- user accounts
- asset accounts
- order routing
- risk checks
- market access
- custody or custodian integration
- statements
- tax reports
- compliance controls
- customer operations

## 1.3 Recommended Direction

For this project, the recommended architecture is:

```text
Brokerage Platform
        |
        |-- Client / Account / KYC / Wallet / Ledger
        |-- Order Management System
        |-- Risk Management System
        |-- Execution Management System
        |-- Market Data System
        |-- Clearing & Settlement System
        |-- Compliance & Reporting System
        |
        |-- Optional Internal Matching Engine
        |-- Optional External Exchange / Liquidity Provider Connectivity
```

This allows the system to support two modes:

1. **Broker Mode**: route orders to external venues.
2. **Internal Trading Venue Mode**: match orders internally through the internal matching engine.

---

# 2. High-Level System Architecture

```text
┌─────────────────────────────────────────────────────────────────────┐
│                          CLIENT LAYER                               │
│ Web App / Mobile App / Desktop Client / Open API / Admin Portal       │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                          ACCESS LAYER                               │
│ API Gateway / WAF / Auth / Rate Limit / Session / Device Binding      │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        BUSINESS SERVICE LAYER                        │
│ User / KYC / Account / Funding / Order / Risk / Portfolio / Report    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         TRADING CORE LAYER                           │
│ OMS / RMS / EMS / Sequencer / Matching Engine / Market Data Engine    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    POST-TRADE & FINANCIAL LAYER                      │
│ Clearing / Settlement / Ledger / Custody / Reconciliation / Fees      │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    COMPLIANCE & OPERATIONS LAYER                     │
│ AML / Surveillance / Audit / Regulatory Reports / Admin / Monitoring  │
└─────────────────────────────────────────────────────────────────────┘
```

---

# 3. Core Design Principles

## 3.1 Separation of Business and Trading Core

Business systems should not directly mutate trading engine state.

Trading core should be isolated from:

- user profile changes
- frontend sessions
- KYC workflows
- notification services
- customer support tools
- admin dashboards

## 3.2 Event-Driven Architecture

Every important state transition should produce an event.

Examples:

- USER_REGISTERED
- KYC_APPROVED
- ACCOUNT_OPENED
- DEPOSIT_CONFIRMED
- ORDER_ACCEPTED
- ORDER_REJECTED
- ORDER_EXECUTED
- POSITION_UPDATED
- CASH_BALANCE_UPDATED
- SETTLEMENT_COMPLETED
- WITHDRAWAL_APPROVED

## 3.3 Strong Financial Consistency

For financial state, the system must prioritize correctness over convenience.

Recommended rules:

- use double-entry ledger
- avoid direct balance overwrite
- use append-only financial journal
- use idempotency key for all money-moving operations
- never trust frontend-calculated values
- reconcile with external banks, custodians, exchanges, and payment channels

## 3.4 Hot Path vs Cold Path

The system should separate latency-sensitive paths from normal business paths.

### Hot Path

- order validation
- pre-trade risk
- sequencing
- matching
- execution report generation
- market data fanout

### Cold Path

- KYC
- reports
- statements
- customer service
- analytics
- compliance case review
- admin management

---

# 4. Recommended Technology Stack

## 4.1 Core Trading Components

| Component | Recommended Language | Reason |
|---|---:|---|
| Matching Engine | C++ | low latency, deterministic memory control |
| Sequencer | C++ / Rust | strict ordering and performance |
| Market Data Engine | C++ / Rust | high throughput |
| Pre-Trade Risk Engine | C++ / Go | performance + business flexibility |
| OMS Core | Go / C++ | balance between speed and maintainability |
| EMS | Go / C++ | venue connectivity and order routing |

## 4.2 Business Services

| Component | Recommended Language |
|---|---:|
| User Service | Go / Java |
| KYC Service | Go / Java |
| Account Service | Go |
| Funding Service | Go |
| Ledger Service | Go / Java |
| Reporting Service | Go / Python |
| Admin Backend | Go / Java |
| Notification Service | Go / Node.js |

## 4.3 Storage

| Data Type | Recommended Storage |
|---|---|
| User profile | PostgreSQL |
| Account state | PostgreSQL |
| Ledger journal | PostgreSQL / FoundationDB |
| Event stream | Kafka / Redpanda / NATS JetStream |
| Hot order book state | In-memory + snapshot |
| Market data time series | ClickHouse / QuestDB |
| Audit logs | Object storage + immutable log |
| Cache | Redis / DragonflyDB |

## 4.4 Infrastructure

- Linux
- Docker / Kubernetes for business services
- bare-metal or dedicated nodes for ultra-low-latency trading core
- Prometheus + Grafana
- OpenTelemetry
- Loki / ELK
- Vault / HSM / KMS
- PostgreSQL high availability
- Kafka / Redpanda cluster
- object storage for archived audit logs

---

# 5. Domain Modules

## 5.1 User & Identity Service

Responsibilities:

- user registration
- login
- password management
- MFA
- device binding
- session management
- account recovery
- user status management

Key states:

```text
REGISTERED
EMAIL_VERIFIED
PHONE_VERIFIED
KYC_PENDING
ACTIVE
SUSPENDED
CLOSED
```

Important APIs:

```text
POST /users/register
POST /auth/login
POST /auth/mfa/verify
POST /auth/logout
GET  /users/me
PATCH /users/me
```

Security requirements:

- MFA for withdrawals
- device fingerprinting
- suspicious login detection
- session revocation
- login audit logs

---

## 5.2 KYC / KYB Service

Responsibilities:

- identity verification
- document verification
- sanctions screening
- PEP screening
- address verification
- risk scoring
- account approval workflow

KYC states:

```text
NOT_STARTED
SUBMITTED
UNDER_REVIEW
APPROVED
REJECTED
EXPIRED
REVERIFY_REQUIRED
```

Events:

```text
KYC_SUBMITTED
KYC_APPROVED
KYC_REJECTED
KYC_REVERIFY_REQUIRED
```

Design note:

KYC should not block the whole user system. It should only control what actions the user is allowed to perform.

Example permission matrix:

| KYC Status | Login | View Market | Deposit | Trade | Withdraw |
|---|---:|---:|---:|---:|---:|
| NOT_STARTED | Yes | Yes | No | No | No |
| SUBMITTED | Yes | Yes | Limited | No | No |
| APPROVED | Yes | Yes | Yes | Yes | Yes |
| REJECTED | Yes | Yes | No | No | No |

---

## 5.3 Account Service

Responsibilities:

- brokerage account creation
- account status management
- account type management
- permission management
- trading region restrictions
- asset permission control

Account types:

```text
CASH_ACCOUNT
MARGIN_ACCOUNT
INSTITUTIONAL_ACCOUNT
API_TRADING_ACCOUNT
PAPER_TRADING_ACCOUNT
```

Account status:

```text
PENDING
ACTIVE
RESTRICTED
LIQUIDATION_ONLY
SUSPENDED
CLOSED
```

Core data model:

```sql
CREATE TABLE brokerage_accounts (
    account_id        BIGSERIAL PRIMARY KEY,
    user_id           BIGINT NOT NULL,
    account_type      VARCHAR(32) NOT NULL,
    account_status    VARCHAR(32) NOT NULL,
    base_currency     VARCHAR(16) NOT NULL,
    created_at        TIMESTAMP NOT NULL,
    updated_at        TIMESTAMP NOT NULL
);
```

---

## 5.4 Funding Service

Responsibilities:

- deposits
- withdrawals
- internal transfers
- payment channel integration
- bank account binding
- crypto wallet address management
- funding risk checks

Funding transaction states:

```text
CREATED
PENDING_REVIEW
PROCESSING
CONFIRMED
FAILED
CANCELED
REVERSED
```

Important rules:

- every deposit must create ledger entries
- every withdrawal must reserve funds before submission
- withdrawal approval should be separated from withdrawal execution
- external payment status must be reconciled
- idempotency is mandatory

Example withdrawal flow:

```text
User Request
    ↓
Permission Check
    ↓
Balance Availability Check
    ↓
Risk & AML Check
    ↓
Fund Reservation
    ↓
Manual / Automatic Approval
    ↓
Payment Execution
    ↓
External Confirmation
    ↓
Ledger Settlement
```

---

## 5.5 Product & Instrument Service

Responsibilities:

- instrument definition
- trading pair configuration
- tick size
- lot size
- trading hours
- fee model
- margin parameters
- symbol status

Instrument types:

```text
STOCK
ETF
FUTURE
OPTION
CRYPTO_SPOT
FX
CFD
BOND
```

Instrument status:

```text
PRE_OPEN
OPEN
HALTED
AUCTION
CLOSED
DELISTED
```

Core fields:

```sql
CREATE TABLE instruments (
    instrument_id      BIGSERIAL PRIMARY KEY,
    symbol             VARCHAR(64) NOT NULL UNIQUE,
    asset_class         VARCHAR(32) NOT NULL,
    base_asset          VARCHAR(32),
    quote_asset         VARCHAR(32),
    tick_size           NUMERIC(38, 18) NOT NULL,
    lot_size            NUMERIC(38, 18) NOT NULL,
    min_order_qty       NUMERIC(38, 18) NOT NULL,
    max_order_qty       NUMERIC(38, 18),
    trading_status      VARCHAR(32) NOT NULL,
    created_at          TIMESTAMP NOT NULL,
    updated_at          TIMESTAMP NOT NULL
);
```

---

# 6. Trading Core Architecture

## 6.1 Order Management System

The OMS is the central business-level order state manager.

Responsibilities:

- receive order requests
- validate order format
- assign client order ID mapping
- maintain order lifecycle
- communicate with risk engine
- route accepted orders to EMS or internal matching engine
- receive execution reports
- update order state
- emit order events

Order states:

```text
NEW_REQUEST
VALIDATED
RISK_CHECKING
ACCEPTED
PARTIALLY_FILLED
FILLED
CANCEL_PENDING
CANCELED
REJECTED
EXPIRED
FAILED
```

Order types:

```text
MARKET
LIMIT
STOP
STOP_LIMIT
POST_ONLY
IOC
FOK
GTC
GTD
```

Order request structure:

```cpp
struct NewOrderRequest {
    uint64_t account_id;
    uint64_t client_order_id;
    uint64_t instrument_id;
    Side side;
    OrderType order_type;
    TimeInForce tif;
    int64_t price;
    int64_t quantity;
    int64_t display_quantity;
    uint64_t timestamp_ns;
};
```

Order event structure:

```cpp
struct OrderEvent {
    uint64_t sequence_id;
    uint64_t order_id;
    uint64_t account_id;
    uint64_t instrument_id;
    OrderEventType event_type;
    int64_t price;
    int64_t quantity;
    int64_t filled_quantity;
    uint64_t timestamp_ns;
};
```

---

## 6.2 Pre-Trade Risk Management System

Responsibilities:

- account status check
- KYC permission check
- cash availability check
- position availability check
- margin check
- price band check
- max order size check
- max order value check
- fat finger check
- restricted instrument check
- trading session check

Risk result:

```cpp
enum class RiskDecision {
    APPROVED,
    REJECTED,
    MANUAL_REVIEW,
    LIQUIDATION_ONLY
};
```

Risk check pipeline:

```text
Order Request
    ↓
Account Status Check
    ↓
Permission Check
    ↓
Instrument Status Check
    ↓
Trading Session Check
    ↓
Balance / Margin Check
    ↓
Position Limit Check
    ↓
Price / Quantity Limit Check
    ↓
Risk Decision
```

Recommended implementation:

- keep frequently used account risk state in memory
- update risk state from ledger and position events
- use immutable snapshots for deterministic risk decisions
- keep risk decision logs for audit

---

## 6.3 Execution Management System

The EMS is responsible for execution routing.

Execution modes:

1. Internal matching engine
2. External exchange
3. Liquidity provider
4. Smart order router

Responsibilities:

- venue selection
- order routing
- external order ID mapping
- execution report normalization
- retry and cancel handling
- venue session management
- FIX / REST / WebSocket adapter management

Execution venue model:

```text
Venue
    ├── Internal Matching Engine
    ├── External Exchange A
    ├── External Exchange B
    ├── Liquidity Provider
    └── OTC Desk
```

---

## 6.4 Internal Matching Engine

The internal matching engine keeps the V6 isolated design.

Recommended rule:

```text
1 symbol shard = 1 process = 1 CPU core = 1 order book group
```

For a full brokerage system, the matching engine should be optional.

Use it when:

- building an internal trading venue
- supporting paper trading
- supporting internal crossing
- building a simulated exchange
- handling internal liquidity pools

Avoid using it as the only execution layer if the platform is primarily a broker connected to external venues.

---

## 6.5 Global Sequencer

Responsibilities:

- assign monotonic sequence ID
- provide deterministic event order
- support replay
- prevent race conditions
- maintain audit traceability

Sequence domains:

```text
GLOBAL_SEQUENCE
ORDER_SEQUENCE
TRADE_SEQUENCE
LEDGER_SEQUENCE
MARKET_DATA_SEQUENCE
```

In a large system, one global sequencer may become a bottleneck. Recommended approach:

- global sequence for audit
- local sequence per shard for hot path
- deterministic merge for replay
- strict ordering inside each instrument shard

---

## 6.6 Market Data System

Responsibilities:

- receive external market data
- normalize feed formats
- maintain order book snapshots
- generate L1 / L2 / L3 data
- distribute market data to clients
- provide historical market data storage

Market data types:

```text
TRADE_TICK
BEST_BID_ASK
ORDER_BOOK_DELTA
ORDER_BOOK_SNAPSHOT
KLINE
REFERENCE_PRICE
INDEX_PRICE
```

Recommended architecture:

```text
External Feeds
    ↓
Feed Handler
    ↓
Normalizer
    ↓
Market Data Sequencer
    ↓
In-Memory Book Builder
    ↓
Fanout Service
    ↓
WebSocket / UDP / Internal Bus
```

---

# 7. Post-Trade Architecture

## 7.1 Position Service

Responsibilities:

- maintain real-time position
- track average cost
- track realized PnL
- track unrealized PnL
- track available quantity
- support short positions if enabled
- provide position snapshots

Position model:

```sql
CREATE TABLE positions (
    account_id          BIGINT NOT NULL,
    instrument_id       BIGINT NOT NULL,
    total_qty           NUMERIC(38, 18) NOT NULL,
    available_qty       NUMERIC(38, 18) NOT NULL,
    locked_qty          NUMERIC(38, 18) NOT NULL,
    avg_price           NUMERIC(38, 18) NOT NULL,
    realized_pnl        NUMERIC(38, 18) NOT NULL,
    updated_at          TIMESTAMP NOT NULL,
    PRIMARY KEY (account_id, instrument_id)
);
```

---

## 7.2 Double-Entry Ledger Service

The ledger is the financial source of truth.

Important rule:

> Account balance must be derived from ledger entries, not manually overwritten.

Ledger entry example:

```text
Deposit 1000 USD

Debit:  Broker Bank Clearing Account     +1000 USD
Credit: Client Cash Account              +1000 USD
```

Trade example:

```text
Buy 10 shares of XYZ at 100 USD

Debit:  Client Securities Position       +10 XYZ
Credit: Client Cash Account              -1000 USD
Debit:  Broker Commission Receivable     +Fee
Credit: Client Cash Account              -Fee
```

Core table:

```sql
CREATE TABLE ledger_entries (
    entry_id            BIGSERIAL PRIMARY KEY,
    transaction_id      UUID NOT NULL,
    account_id          BIGINT,
    ledger_account      VARCHAR(128) NOT NULL,
    asset               VARCHAR(32) NOT NULL,
    debit_amount        NUMERIC(38, 18) NOT NULL DEFAULT 0,
    credit_amount       NUMERIC(38, 18) NOT NULL DEFAULT 0,
    business_type       VARCHAR(64) NOT NULL,
    reference_id        VARCHAR(128),
    created_at          TIMESTAMP NOT NULL
);
```

Ledger requirements:

- immutable entries
- balanced transaction groups
- idempotency key
- audit trace
- reconciliation support
- correction through reversing entries only

---

## 7.3 Clearing Service

Responsibilities:

- trade confirmation
- trade allocation
- netting
- fee calculation
- clearing file generation
- clearing status tracking

Clearing states:

```text
PENDING
CONFIRMED
NETTED
SUBMITTED
ACCEPTED
REJECTED
FAILED
```

---

## 7.4 Settlement Service

Responsibilities:

- cash settlement
- securities settlement
- crypto asset settlement
- settlement instruction generation
- settlement status tracking
- failed settlement handling

Settlement states:

```text
PENDING
PROCESSING
SETTLED
FAILED
REVERSED
```

---

## 7.5 Custody Service

Responsibilities:

- asset custody records
- external custodian integration
- asset transfer tracking
- wallet / bank account mapping
- proof of reserve support if needed
- custody reconciliation

---

## 7.6 Reconciliation Service

Reconciliation is critical for brokerage systems.

Reconciliation targets:

- internal ledger vs bank statement
- internal position vs custodian position
- internal order vs external venue order
- internal trade vs clearing report
- internal cash balance vs payment provider balance
- internal crypto balance vs blockchain balance

Reconciliation frequency:

| Target | Frequency |
|---|---:|
| Trading events | near real-time |
| External exchange orders | intraday |
| Bank settlement | daily |
| Custodian positions | daily |
| Ledger balance | daily |
| Regulatory reports | daily / monthly |

---

# 8. Compliance & Risk Control

## 8.1 AML Monitoring

Responsibilities:

- suspicious deposit detection
- suspicious withdrawal detection
- high-risk country screening
- velocity checks
- structured transaction detection
- sanctions list monitoring

Example AML rules:

```text
Large deposit followed by immediate withdrawal
Multiple accounts using same device
Frequent failed login attempts before withdrawal
Repeated small transfers below reporting threshold
New account trading high-risk products immediately
```

---

## 8.2 Trade Surveillance

Responsibilities:

- wash trading detection
- spoofing detection
- layering detection
- pump-and-dump detection
- insider-like pattern detection
- self-trading detection

Surveillance event examples:

```text
SURVEILLANCE_ALERT_CREATED
SURVEILLANCE_CASE_OPENED
SURVEILLANCE_CASE_ESCALATED
SURVEILLANCE_CASE_CLOSED
```

---

## 8.3 Regulatory Reporting

Possible reports:

- order audit trail
- trade reports
- client asset reports
- suspicious activity reports
- tax statements
- transaction statements
- daily capital reports
- margin reports

Design recommendation:

Do not generate regulatory reports directly from operational databases.

Use:

```text
Event Stream → Reporting Warehouse → Report Generator → Immutable Archive
```

---

# 9. Admin & Operations System

## 9.1 Admin Portal

Core functions:

- user search
- account status management
- KYC review
- deposit review
- withdrawal approval
- order search
- trade search
- ledger search
- position search
- risk parameter management
- instrument management
- fee configuration
- compliance case management

## 9.2 Permission Model

Use RBAC + approval workflow.

Example roles:

```text
SUPER_ADMIN
COMPLIANCE_OFFICER
KYC_REVIEWER
RISK_MANAGER
OPERATIONS_STAFF
FINANCE_STAFF
READ_ONLY_AUDITOR
CUSTOMER_SUPPORT
```

Sensitive operations require:

- maker-checker approval
- audit log
- reason code
- optional MFA
- operation replayability

---

# 10. Data Architecture

## 10.1 Data Classification

| Data Type | Consistency Requirement | Storage |
|---|---:|---|
| Order state | high | OMS DB + event stream |
| Trade event | very high | event log + DB |
| Ledger | extremely high | immutable journal |
| Market data | high volume | time-series DB |
| User profile | normal | relational DB |
| KYC document | high security | encrypted object storage |
| Audit log | immutable | WORM/object storage |
| Reports | immutable | object storage |

---

## 10.2 Event Bus Design

Recommended event topics:

```text
user.events
kyc.events
account.events
funding.events
order.events
execution.events
trade.events
position.events
ledger.events
risk.events
marketdata.events
compliance.events
audit.events
```

Event envelope:

```json
{
  "event_id": "uuid",
  "event_type": "ORDER_ACCEPTED",
  "aggregate_id": "order_id",
  "account_id": "account_id",
  "sequence_id": 10000001,
  "timestamp_ns": 1710000000000000000,
  "source_service": "oms",
  "schema_version": 1,
  "payload": {}
}
```

---

## 10.3 Database Boundary

Each domain should own its own database.

Bad design:

```text
All services directly read and write the same database.
```

Better design:

```text
Each service owns its database.
Other services consume events or call APIs.
```

---

# 11. Security Architecture

## 11.1 User Security

- password hashing with modern algorithms
- MFA
- device binding
- withdrawal address whitelist
- login anomaly detection
- session revocation
- IP risk scoring

## 11.2 System Security

- TLS everywhere
- mTLS for internal services
- HSM / KMS for sensitive keys
- encrypted secrets
- least privilege IAM
- database encryption
- immutable audit logs
- strict production access control

## 11.3 API Security

- rate limiting
- request signing for API trading
- timestamp validation
- nonce / idempotency key
- replay attack protection
- IP whitelist for institutional users

API request signature example:

```text
signature = HMAC_SHA256(secret, method + path + timestamp + body)
```

---

# 12. Observability

## 12.1 Metrics

Important metrics:

- order latency
- risk check latency
- matching latency
- API latency
- event lag
- rejected order ratio
- failed withdrawal ratio
- ledger imbalance count
- reconciliation mismatch count
- database replication lag
- market data fanout delay

## 12.2 Logs

Log types:

- application logs
- audit logs
- security logs
- order lifecycle logs
- risk decision logs
- admin operation logs
- reconciliation logs

## 12.3 Tracing

Use distributed tracing for cold-path services.

For hot-path trading systems, avoid heavy tracing inside critical execution loops. Use lightweight binary logs or ring-buffer diagnostics.

---

# 13. High Availability & Disaster Recovery

## 13.1 Availability Design

- active-active API gateway
- active-passive trading shard
- hot standby matching engine
- replicated event log
- database primary-replica
- multi-zone deployment
- automated failover
- manual emergency control

## 13.2 Recovery Design

Recovery sources:

- WAL
- event log
- snapshots
- ledger journal
- external reconciliation data

Recovery process:

```text
Load Latest Snapshot
    ↓
Replay Event Log
    ↓
Rebuild Order Book / Position / Balance
    ↓
Run Consistency Check
    ↓
Resume Service
```

## 13.3 Disaster Recovery Targets

| Component | RPO | RTO |
|---|---:|---:|
| Trading Core | seconds | minutes |
| Ledger | near zero | minutes |
| User Service | minutes | minutes |
| Market Data History | minutes | hours |
| Reporting | hours | hours |

---

# 14. Performance Design

## 14.1 Latency Tiers

| Path | Target |
|---|---:|
| Matching hot path | microseconds |
| Risk hot path | microseconds to low milliseconds |
| API order submission | low milliseconds |
| Market data fanout | milliseconds |
| Funding / KYC | seconds to minutes |

## 14.2 Optimization Rules

- avoid locks in matching engine
- avoid heap allocation in hot path
- use preallocated memory pools
- use CPU affinity
- use append-only binary logs
- separate control plane from data plane
- avoid synchronous remote calls in hot path
- prefer event-driven asynchronous workflows

---

# 15. Suggested Process Isolation Model

```text
broker-api-gateway
broker-auth-service
broker-user-service
broker-kyc-service
broker-account-service
broker-funding-service
broker-oms-service
broker-risk-service
broker-ems-service
broker-sequencer
broker-matching-shard-001
broker-matching-shard-002
broker-marketdata-feedhandler
broker-marketdata-fanout
broker-position-service
broker-ledger-service
broker-clearing-service
broker-settlement-service
broker-custody-service
broker-recon-service
broker-compliance-service
broker-reporting-service
broker-admin-service
broker-notification-service
```

---

# 16. Suggested Repository Structure

```text
brokerage-system/
├── docs/
│   ├── architecture/
│   ├── api/
│   ├── database/
│   ├── operations/
│   └── compliance/
├── services/
│   ├── api-gateway/
│   ├── auth-service/
│   ├── user-service/
│   ├── kyc-service/
│   ├── account-service/
│   ├── funding-service/
│   ├── oms-service/
│   ├── risk-service/
│   ├── ems-service/
│   ├── ledger-service/
│   ├── position-service/
│   ├── clearing-service/
│   ├── settlement-service/
│   ├── custody-service/
│   ├── compliance-service/
│   ├── reporting-service/
│   └── admin-service/
├── trading-core/
│   ├── sequencer/
│   ├── matching-engine/
│   ├── market-data-engine/
│   ├── ipc/
│   ├── memory/
│   └── benchmark/
├── libs/
│   ├── common/
│   ├── protocol/
│   ├── event-schema/
│   ├── risk-model/
│   └── utils/
├── deploy/
│   ├── docker/
│   ├── k8s/
│   └── bare-metal/
├── scripts/
└── tests/
```

---

# 17. Development Roadmap

## Phase 1: Minimal Usable Brokerage Core

Goal:

Build a usable internal paper-trading brokerage system.

Features:

- user registration
- account creation
- instrument management
- simulated deposit
- simple order placement
- pre-trade risk check
- internal matching engine
- position update
- basic ledger entries
- order history
- trade history
- simple admin portal

Recommended scope:

```text
Auth + Account + OMS + Risk + Matching + Position + Ledger
```

---

## Phase 2: Realistic Trading Platform

Features:

- real market data
- external exchange adapter
- EMS
- order routing
- advanced order types
- deposit / withdrawal workflow
- fee engine
- statements
- reconciliation
- monitoring dashboard

---

## Phase 3: Compliance & Operations Upgrade

Features:

- KYC workflow
- AML rules
- trade surveillance
- admin approval workflow
- immutable audit log
- regulatory reporting framework
- customer support tools

---

## Phase 4: Production Hardening

Features:

- HA deployment
- failover
- disaster recovery
- security hardening
- penetration testing
- performance benchmarking
- chaos testing
- operational runbooks

---

## Phase 5: Institutional-Grade Platform

Features:

- FIX gateway
- institutional API
- smart order router
- margin trading
- multi-currency ledger
- custodian integration
- risk model upgrade
- portfolio analytics
- multi-region deployment

---

# 18. MVP Implementation Recommendation

For your current stage, avoid building everything at once.

Recommended MVP modules:

```text
1. Auth Service
2. Account Service
3. Instrument Service
4. OMS
5. Risk Engine
6. Matching Engine
7. Position Service
8. Ledger Service
9. Market Data Simulator
10. Admin Portal
```

Do not start with:

- full regulatory reporting
- real bank integration
- real exchange integration
- margin system
- options trading
- complex smart order routing

These should be added after the internal trading loop is stable.

---

# 19. Core End-to-End Order Flow

## 19.1 Buy Limit Order Flow

```text
Client
  ↓
API Gateway
  ↓
Auth / Permission Check
  ↓
OMS
  ↓
Pre-Trade Risk
  ↓
Sequencer
  ↓
Matching Engine / EMS
  ↓
Execution Report
  ↓
OMS State Update
  ↓
Position Update
  ↓
Ledger Entry
  ↓
Market Data Update
  ↓
Client Notification
```

## 19.2 Withdrawal Flow

```text
Client
  ↓
API Gateway
  ↓
Auth + MFA
  ↓
Funding Service
  ↓
AML Check
  ↓
Balance Reservation
  ↓
Approval Workflow
  ↓
Payment Provider / Bank / Wallet
  ↓
Confirmation
  ↓
Ledger Entry
  ↓
Notification
```

## 19.3 Trade Settlement Flow

```text
Trade Executed
  ↓
Trade Capture
  ↓
Fee Calculation
  ↓
Position Update
  ↓
Ledger Posting
  ↓
Clearing Instruction
  ↓
Settlement Confirmation
  ↓
Reconciliation
```

---

# 20. Testing Strategy

## 20.1 Unit Tests

- order validation
- risk rules
- ledger balancing
- fee calculation
- position calculation
- matching algorithm

## 20.2 Integration Tests

- order flow
- cancel flow
- deposit flow
- withdrawal flow
- trade settlement flow
- reconciliation flow

## 20.3 Performance Tests

- matching latency
- order throughput
- market data throughput
- risk check latency
- event bus lag
- database write throughput

## 20.4 Failure Tests

- matching engine crash
- sequencer restart
- event bus failure
- database failover
- duplicated event
- delayed external confirmation
- inconsistent reconciliation result

---

# 21. Key Risks and Mitigations

| Risk | Description | Mitigation |
|---|---|---|
| Ledger inconsistency | wrong cash or asset balance | double-entry ledger + reconciliation |
| Order state mismatch | OMS and execution venue disagree | event sourcing + reconciliation |
| Over-complexity | too many modules too early | phased roadmap |
| Latency degradation | business services affect hot path | isolate hot path |
| Compliance gaps | missing audit or AML controls | compliance-by-design |
| Security breach | account takeover or key leakage | MFA + HSM/KMS + audit |
| Data loss | corrupted state or missing events | WAL + snapshots + replay |
| External venue failure | orders cannot execute | EMS retry + venue health management |

---

# 22. Recommended Next Version: V8

The next architecture version should focus on implementation-level design.

Recommended V8 topics:

1. database schema design
2. event schema design
3. service API contracts
4. C++ matching engine internal design
5. Go service skeleton
6. risk rule engine design
7. ledger transaction model
8. deployment topology
9. monitoring dashboard
10. MVP development milestones

---

# 23. Final Architecture Summary

The V7 architecture transforms the previous isolated trading engine design into a complete brokerage system blueprint.

The most important upgrades are:

- brokerage domain separation
- complete account and funding system
- OMS / RMS / EMS separation
- optional internal matching engine
- double-entry ledger as source of truth
- post-trade clearing and settlement
- custody and reconciliation
- compliance and surveillance
- admin operations platform
- phased MVP-to-production roadmap

The key engineering principle is:

> Build the trading core as a deterministic isolated system, and build the brokerage platform around it as an event-driven financial system.


---

# Appendix A: Original V6 Design Reference


# V6 Isolated Trading System Architecture (Binance / NASDAQ Grade)

---

# 1. Overview

V6 architecture introduces a **fully isolated, fault-domain-separated, production-grade trading system design**.

It upgrades previous versions by enforcing:

- Process-level isolation (hard separation)
- Thread-level isolation (deterministic execution)
- Memory-domain isolation (latency-tiered memory)
- Data-flow isolation (unidirectional event streaming)
- Failure-domain isolation (blast radius control)

---

# 2. Design Goals

## Core Objectives

- Microsecond-level deterministic execution
- Zero cross-module state contamination
- Full fault isolation (no cascading failures)
- Sharded and independently recoverable engines
- Event-sourced system with replay guarantee
- Strict separation of concerns (SoC)
- Kernel-bypass-ready architecture (optional extension)

---

# 3. Isolation Architecture Model

```
                    ┌────────────────────────┐
                    │      CLIENT LAYER      │
                    └──────────┬─────────────┘
                               ↓
        ┌─────────────────────────────────────────┐
        │         API GATEWAY CLUSTER            │
        │  (Stateless / Fully Isolated Nodes)    │
        └──────────┬──────────────────────────────┘
                   ↓
     ┌──────────────────────────────────────────────┐
     │      ORDER GATE ISOLATION LAYER             │
     │  (Rate limit / validation / buffering)      │
     └──────────┬───────────────────────────────────┘
                ↓
     ┌──────────────────────────────────────────────┐
     │     PRE-TRADE RISK ENGINE (ISOLATED)        │
     │     - No dependency on matching engine      │
     └──────────┬───────────────────────────────────┘
                ↓
     ┌──────────────────────────────────────────────┐
     │   GLOBAL SEQUENCER (STRICT SINGLE SOURCE)    │
     │   - Total ordering of all events             │
     └──────────┬───────────────────────────────────┘
                ↓
     ┌──────────────────────────────────────────────┐
     │ SHARDED MATCHING ENGINE CLUSTER (ISOLATED)   │
     │  - 1 shard = 1 process = 1 CPU domain        │
     └──────────┬───────────────────────────────────┘
                ↓
     ┌──────────────────────────────────────────────┐
     │   MARKET DATA FANOUT CLUSTER (ISOLATED)      │
     └──────────┬───────────────────────────────────┘
                ↓
     ┌──────────────────────────────────────────────┐
     │   CLEARING / SETTLEMENT ENGINE (ISOLATED)    │
     └──────────────────────────────────────────────┘
```

---

# 4. Core Isolation Principles

---

## 4.1 Process Isolation (Hard Boundary)

Each subsystem runs as an independent OS process:

- API Gateway Process
- Risk Engine Process
- Sequencer Process
- Matching Engine Process (per shard)
- Market Data Process
- Clearing Process

### Benefits:

- Crash containment
- Independent scaling
- Independent restart
- Memory protection

---

## 4.2 Thread Isolation (Deterministic Execution)

Inside each process:

- Matching engine = single-threaded per shard
- Risk engine = worker pool (no shared state)
- IO = dedicated threads

### Rule:

> One writer per critical data structure

---

## 4.3 Memory Isolation (Tiered Model)

### Memory Zones:

```
HOT PATH    → slab allocator / hugepages
WARM PATH   → lock-free pools
COLD PATH   → heap / async IO
```

---

## 4.4 Data Flow Isolation

All data flows are:

- Unidirectional
- Immutable once published
- Event-based (no shared mutable state)

---

## 4.5 Failure Domain Isolation

Each shard is independent:

- shard failure ≠ system failure
- risk engine failure ≠ matching failure
- market data failure ≠ trading stop

---

# 5. Matching Engine (Fully Isolated Design)

## 5.1 Architecture Rule

```
1 shard = 1 process = 1 CPU core = 1 order book
```

---

## 5.2 Execution Model

```
Order In → Validate → Sequence → Match → Emit Event
```

---

## 5.3 Deterministic Execution

- No locks
- No shared state
- No async mutation inside shard

---

# 6. Sequencer (Global Ordering Layer)

## Responsibilities:

- Assign global monotonic sequence ID
- Guarantee deterministic replay order
- Remove race conditions between shards

---

## Model:

```
All Orders → Sequencer → Ordered Event Stream
```

---

# 7. Event Sourcing Backbone

All system state is derived from events:

## Event Types:

- ORDER_CREATED
- ORDER_REJECTED
- ORDER_MATCHED
- TRADE_EXECUTED
- ORDER_CANCELED

---

## Benefits:

- Full system replay
- Audit compliance
- Crash recovery
- Debug determinism

---

# 8. Memory Management System

---

## 8.1 Slab Allocator (Hot Path)

- Fixed-size allocation
- Cache aligned
- No fragmentation

---

## 8.2 HugePage Allocator

Used for:

- Order books
- Market data buffers

---

## 8.3 Lock-Free Pool

- Multi-thread safe
- Atomic freelist
- Zero contention design

---

# 9. Threading & CPU Affinity Model

## CPU Binding Strategy:

| Component | CPU Model |
|----------|-----------|
| Matching Engine | Dedicated core |
| Risk Engine | Worker pool |
| Sequencer | Single core |
| Market Data | Fanout cores |

---

## Rule:

> No context switching in hot path

---

# 10. Inter-Process Communication (IPC)

- Shared memory ring buffers (hot path)
- ZeroMQ (event distribution)
- gRPC (control plane only)
- UDP multicast (market data)

---

# 11. High Availability Design

---

## 11.1 Active-Active Shards

Each shard has:

- Primary engine
- Hot standby replica

---

## 11.2 Failover Strategy

```
Shard Failure → Replay Event Log → Restore State → Resume
```

---

# 12. Persistence Layer

- Append-only WAL (write-ahead log)
- Periodic snapshots
- Full replay engine

---

# 13. Market Data System

- L1 / L2 / L3 feeds
- Snapshot + delta updates
- Zero-copy fanout pipeline

---

# 14. Clearing & Settlement System

- Double-entry ledger
- Margin tracking
- Position reconciliation
- Netting engine

---

# 15. Security Model

- TLS encrypted communication
- HSM key management
- Role-based access control
- Immutable audit logs

---

# 16. Performance Targets

| Metric | Target |
|--------|--------|
| Order Latency | < 10–30 μs |
| Throughput | 5M+ orders/sec |
| Market Data | 20M ticks/sec |

---

# 17. Advanced Optimizations

- Kernel bypass (DPDK / RDMA)
- SIMD-based matching
- Cache-line optimization
- Branchless execution paths

---

# 18. Summary

V6 introduces a **fully isolated trading architecture**, achieving:

- Hard process isolation
- Deterministic shard execution
- Event-sourced system state
- Zero shared mutable state
- Production-grade fault containment

This architecture is aligned with:

- Binance exchange core design principles
- NASDAQ matching engine isolation model
- Institutional HFT infrastructure standards

