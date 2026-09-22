#include "ble_keyboard.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

namespace blekb {
namespace {

constexpr uint8_t REPORT_ID = 1;
constexpr uint8_t MOD_SHIFT = 0x02;
constexpr uint8_t SHIFT_FLAG = 0x80;  // marks ASCII_MAP entries that need Shift

// Standard boot-compatible keyboard report (modifiers, reserved, 6 keys) + LED output
uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, REPORT_ID,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

// US layout, ASCII 0x20..0x7E -> HID usage (| SHIFT_FLAG when Shift is needed)
const uint8_t ASCII_MAP[95] = {
    0x2C,              0x1E | SHIFT_FLAG, 0x34 | SHIFT_FLAG, 0x20 | SHIFT_FLAG,  //  !"#
    0x21 | SHIFT_FLAG, 0x22 | SHIFT_FLAG, 0x24 | SHIFT_FLAG, 0x34,               // $%&'
    0x26 | SHIFT_FLAG, 0x27 | SHIFT_FLAG, 0x25 | SHIFT_FLAG, 0x2E | SHIFT_FLAG,  // ()*+
    0x36,              0x2D,              0x37,              0x38,               // ,-./
    0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,                  // 0-9
    0x33 | SHIFT_FLAG, 0x33,              0x36 | SHIFT_FLAG, 0x2E,               // :;<=
    0x37 | SHIFT_FLAG, 0x38 | SHIFT_FLAG, 0x1F | SHIFT_FLAG,                     // >?@
    // A-Z
    0x04 | SHIFT_FLAG, 0x05 | SHIFT_FLAG, 0x06 | SHIFT_FLAG, 0x07 | SHIFT_FLAG, 0x08 | SHIFT_FLAG,
    0x09 | SHIFT_FLAG, 0x0A | SHIFT_FLAG, 0x0B | SHIFT_FLAG, 0x0C | SHIFT_FLAG, 0x0D | SHIFT_FLAG,
    0x0E | SHIFT_FLAG, 0x0F | SHIFT_FLAG, 0x10 | SHIFT_FLAG, 0x11 | SHIFT_FLAG, 0x12 | SHIFT_FLAG,
    0x13 | SHIFT_FLAG, 0x14 | SHIFT_FLAG, 0x15 | SHIFT_FLAG, 0x16 | SHIFT_FLAG, 0x17 | SHIFT_FLAG,
    0x18 | SHIFT_FLAG, 0x19 | SHIFT_FLAG, 0x1A | SHIFT_FLAG, 0x1B | SHIFT_FLAG, 0x1C | SHIFT_FLAG,
    0x1D | SHIFT_FLAG,
    0x2F,              0x31,              0x30,              0x23 | SHIFT_FLAG,  // [\]^
    0x2D | SHIFT_FLAG, 0x35,                                                     // _`
    // a-z
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
    0x2F | SHIFT_FLAG, 0x31 | SHIFT_FLAG, 0x30 | SHIFT_FLAG, 0x35 | SHIFT_FLAG,  // {|}~
};

NimBLECharacteristic *input = nullptr;
volatile bool connected = false;
bool ok = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override { connected = true; }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override { connected = false; }
};

void sendReport(uint8_t modifiers, uint8_t usage) {
    uint8_t report[8] = {modifiers, 0, usage, 0, 0, 0, 0, 0};
    input->setValue(report, sizeof(report));
    input->notify();
}

}  // namespace

bool begin(const char *name) {
    NimBLEDevice::init(name);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bond, no PIN, secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    server->advertiseOnDisconnect(true);

    auto *hid = new NimBLEHIDDevice(server);
    hid->setManufacturer("M5Stack");
    hid->setPnp(0x02, 0x303A, 0x4001, 0x0100);  // Espressif VID
    hid->setHidInfo(0x00, 0x01);
    hid->setReportMap(REPORT_MAP, sizeof(REPORT_MAP));
    hid->setBatteryLevel(100);
    input = hid->getInputReport(REPORT_ID);
    hid->getOutputReport(REPORT_ID);  // LED report, required by some hosts
    bool started = server->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C1);  // HID keyboard
    adv->addServiceUUID(hid->getHidService()->getUUID());
    adv->setName(name);
    ok = started && adv->start();
    return ok;
}

bool isReady() { return ok; }

bool isConnected() { return connected; }

void tapKey(uint8_t usage, uint8_t modifiers) {
    if (!connected) return;
    sendReport(modifiers, usage);
    delay(8);
    sendReport(0, 0);
    delay(8);
}

bool typeChar(char c) {
    if (c < 0x20 || c > 0x7E) return false;
    uint8_t code = ASCII_MAP[c - 0x20];
    tapKey(code & ~SHIFT_FLAG, (code & SHIFT_FLAG) ? MOD_SHIFT : 0);
    return true;
}

}  // namespace blekb
