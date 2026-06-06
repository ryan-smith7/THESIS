  CREATE TABLE telemetry_bme280 (
    id          INT           IDENTITY(1,1)  NOT NULL,
    device_id   VARCHAR(32)   NOT NULL,
    datetime    DATETIME2(3)  NOT NULL,
    utc_valid   BIT           NOT NULL        DEFAULT 1,
    temp_c      REAL          NULL,
    rh_pct      REAL          NULL,
    press_hPa   REAL          NULL,
    CONSTRAINT PK_telemetry_bme280 PRIMARY KEY (id)
);

CREATE INDEX IX_bme280_device_datetime
    ON telemetry_bme280 (device_id, datetime);