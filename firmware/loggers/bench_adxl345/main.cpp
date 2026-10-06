// Breadboard bring-up test: ADXL345 (GY-291) over SPI on the TinyS3[D].
// Streams raw X/Y/Z samples over USB serial for tools/accel-viewer (temporary).
//
// Serial protocol (one line each):
//   #ADXL345 rate=<Hz> g_per_lsb=<scale>   info line, repeated every 2 s
//   D,<x>,<y>,<z>                           one sample, raw counts

#include <Arduino.h>
#include <SPI.h>

// TinyS3[D] onboard pins
constexpr int PIN_NEOPIXEL       = 18;
constexpr int PIN_NEOPIXEL_POWER = 17;

// ADXL345 wiring (TinyS3 default SPI pins)
constexpr int PIN_ADXL_CS   = 34;
constexpr int PIN_ADXL_MOSI = 35;  // GY-291 "SDA"
constexpr int PIN_ADXL_SCK  = 36;  // GY-291 "SCL"
constexpr int PIN_ADXL_MISO = 37;  // GY-291 "SDO"

// ADXL345 registers
constexpr uint8_t REG_DEVID       = 0x00;
constexpr uint8_t REG_THRESH_TAP  = 0x1D;
constexpr uint8_t REG_BW_RATE     = 0x2C;
constexpr uint8_t REG_POWER_CTL   = 0x2D;
constexpr uint8_t REG_DATA_FORMAT = 0x31;
constexpr uint8_t REG_DATAX0      = 0x32;
constexpr uint8_t REG_FIFO_CTL    = 0x38;
constexpr uint8_t REG_FIFO_STATUS = 0x39;

constexpr uint8_t ADXL345_DEVID   = 0xE5;
constexpr float   G_PER_LSB       = 0.0039f;  // full-resolution mode, any range
constexpr int     SAMPLE_RATE_HZ  = 800;
constexpr uint8_t BW_RATE_800HZ   = 0x0D;

// ADXL345 supports SPI mode 3 up to 5 MHz; 1 MHz is plenty for 800 Hz and
// tolerates long breadboard jumpers
const SPISettings adxlSpi(1000000, MSBFIRST, SPI_MODE3);

void adxlWrite(uint8_t reg, uint8_t value) {
    SPI.beginTransaction(adxlSpi);
    digitalWrite(PIN_ADXL_CS, LOW);
    SPI.transfer(reg & 0x3F);
    SPI.transfer(value);
    digitalWrite(PIN_ADXL_CS, HIGH);
    SPI.endTransaction();
}

void adxlRead(uint8_t reg, uint8_t* buf, size_t len) {
    SPI.beginTransaction(adxlSpi);
    digitalWrite(PIN_ADXL_CS, LOW);
    SPI.transfer(0x80 | (len > 1 ? 0x40 : 0x00) | (reg & 0x3F));  // read + multi-byte
    for (size_t i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
    digitalWrite(PIN_ADXL_CS, HIGH);
    SPI.endTransaction();
}

void setPixel(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(PIN_NEOPIXEL, r, g, b);
}

uint32_t samplesSinceInfo = 0;
uint32_t sensorResets = 0;

void printInfo() {
    Serial.printf("#ADXL345 rate=%d g_per_lsb=%.4f samples=%lu resets=%lu\n", SAMPLE_RATE_HZ, G_PER_LSB,
                  (unsigned long)samplesSinceInfo, (unsigned long)sensorResets);
    samplesSinceInfo = 0;
}

void adxlConfigure() {
    adxlWrite(REG_POWER_CTL, 0x00);          // standby while configuring
    adxlWrite(REG_DATA_FORMAT, 0x0B);        // 4-wire SPI, full resolution, +/-16 g
    adxlWrite(REG_BW_RATE, BW_RATE_800HZ);
    adxlWrite(REG_FIFO_CTL, 0x80);           // FIFO stream mode (keeps newest 32 samples)
    adxlWrite(REG_POWER_CTL, 0x08);          // start measuring
}

// Checks the sensor is present and still measuring. A power glitch resets the
// ADXL345 into standby, so reconfigure it if that has happened.
bool adxlCheck() {
    uint8_t id = 0, power = 0;
    adxlRead(REG_DEVID, &id, 1);
    if (id != ADXL345_DEVID) {
        Serial.printf("#ERROR ADXL345 not responding: DEVID = 0x%02X (expected 0xE5). Check wiring.\n", id);
        setPixel(40, 0, 0);  // red
        return false;
    }
    // Write/read-back test on an unused register (THRESH_TAP) to catch SPI corruption
    uint8_t probe = 0;
    adxlWrite(REG_THRESH_TAP, 0x5A);
    adxlRead(REG_THRESH_TAP, &probe, 1);

    adxlRead(REG_POWER_CTL, &power, 1);
    if (power != 0x08 || probe != 0x5A) {
        uint8_t bw = 0, fmt = 0, fifo = 0;
        adxlRead(REG_BW_RATE, &bw, 1);
        adxlRead(REG_DATA_FORMAT, &fmt, 1);
        adxlRead(REG_FIFO_CTL, &fifo, 1);
        Serial.printf("#WARN ADXL345 registers wrong, reconfiguring: POWER_CTL=0x%02X (want 08) "
                      "BW_RATE=0x%02X (want 0D) DATA_FORMAT=0x%02X (want 0B) FIFO_CTL=0x%02X (want 80) "
                      "readback=0x%02X (want 5A)\n", power, bw, fmt, fifo, probe);
        sensorResets++;
        adxlConfigure();
    }
    setPixel(0, 40, 0);  // green
    return true;
}

void setup() {
    Serial.begin(115200);
    // Give up quickly when no PC is listening. Must not be 0: the HWCDC driver's
    // retry counter underflows at 0 and write() then hangs forever once the host
    // closes the port (arduino-esp32 2.0.17).
    Serial.setTxTimeoutMs(5);
    delay(1500);               // give the USB serial monitor time to connect

    pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
    digitalWrite(PIN_NEOPIXEL_POWER, HIGH);

    pinMode(PIN_ADXL_CS, OUTPUT);
    digitalWrite(PIN_ADXL_CS, HIGH);
    SPI.begin(PIN_ADXL_SCK, PIN_ADXL_MISO, PIN_ADXL_MOSI, PIN_ADXL_CS);

    adxlConfigure();
    adxlCheck();
    printInfo();
}

void loop() {
    static uint32_t lastInfo = 0;
    constexpr int FIFO_MAX_ENTRIES = 33;      // 32-level FIFO + output register
    constexpr size_t MAX_LINE = 24;           // "D,-32768,-32768,-32768\n" + NUL
    static char out[FIFO_MAX_ENTRIES * MAX_LINE];

    uint8_t status = 0;
    adxlRead(REG_FIFO_STATUS, &status, 1);
    int entries = status & 0x3F;
    if (entries > FIFO_MAX_ENTRIES) entries = FIFO_MAX_ENTRIES;  // corrupted SPI read

    size_t len = 0;
    for (int i = 0; i < entries; i++) {
        uint8_t raw[6];
        adxlRead(REG_DATAX0, raw, 6);  // each read pops one sample from the FIFO
        int16_t x = (int16_t)(raw[1] << 8 | raw[0]);
        int16_t y = (int16_t)(raw[3] << 8 | raw[2]);
        int16_t z = (int16_t)(raw[5] << 8 | raw[4]);
        int n = snprintf(out + len, sizeof(out) - len, "D,%d,%d,%d\n", x, y, z);
        if (n > 0 && len + n < sizeof(out)) len += n;
    }
    if (len) Serial.write((const uint8_t*)out, len);
    samplesSinceInfo += entries;

    if (millis() - lastInfo > 2000) {
        lastInfo = millis();
        printInfo();
        adxlCheck();
    }

    delay(5);  // FIFO holds 40 ms at 800 Hz; drain well before it fills
}
