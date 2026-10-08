-- Isolated DB-viewer evidence, not a trading or production warehouse fixture.
CREATE SCHEMA research;
CREATE TABLE research.observations (
    id BIGINT,
    amount DECIMAL(38, 12),
    session_date DATE,
    available_at TIMESTAMP,
    symbol VARCHAR,
    optional_value VARCHAR,
    empty_value VARCHAR
);
INSERT INTO research.observations VALUES
    (9223372036854775807, 12345678901234567890123456.123456789012,
     DATE '2022-01-03', TIMESTAMP '2022-01-03 22:00:00', 'A"B', NULL, ''),
    (-9223372036854775808, -0.000000000001,
     DATE '2022-01-04', TIMESTAMP '2022-01-04 22:00:00', 'NULL', 'NULL', ''),
    (0, 0, DATE '2022-01-05', TIMESTAMP '2022-01-05 22:00:00', 'Ω', 'present', '');
CREATE TABLE research."quoted""table" ("odd""column" VARCHAR);
INSERT INTO research."quoted""table" VALUES ('quoted identifier survives');
CREATE VIEW research.observation_view AS SELECT symbol, session_date FROM research.observations;
