# ESP32-C3 Matter over Wi-Fi – Relay GPIO5

Firmware ESP-IDF + [esp-matter](https://github.com/espressif/esp-matter) cho ESP32-C3, tạo thiết bị Matter
**On/Off Plug-in Unit** (ổ cắm/công tắc) điều khiển **relay ở GPIO5**, kết nối qua **Wi-Fi**,
commissioning qua **BLE**.

| Chức năng | Chân |
|---|---|
| Relay (active-high mặc định) | GPIO5 |
| Nút BOOT – nhấn ngắn: bật/tắt relay | GPIO9 |
| Nút BOOT – giữ 5 s rồi thả: factory reset | GPIO9 |

Trạng thái relay được lưu NVS và khôi phục sau khi mất điện.

## Cấu hình

`idf.py menuconfig` → **Relay Configuration**:

- `RELAY_GPIO` (mặc định 5)
- `RELAY_ACTIVE_LEVEL` (1 = active-high, 0 = active-low; module relay opto rẻ tiền thường là active-low)

Hoặc sửa trực tiếp `sdkconfig.defaults.esp32c3` rồi xoá `sdkconfig` để áp dụng lại.

## Build

Yêu cầu: ESP-IDF v6.0.2 + esp-matter release/v1.6 (đã `export.sh` cả hai). Cách nhanh nhất là dùng Docker:

```bash
docker run --rm -it -v $PWD:/project \
  espressif/esp-matter:release-v1.6.1_idf_v6.0.2 \
  bash -c "cd /project && idf.py set-target esp32c3 && idf.py build"
```

Hoặc cài local:

```bash
. ~/esp/esp-idf/export.sh
. ~/esp/esp-matter/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 erase-flash flash monitor
```

GitHub Actions (`.github/workflows/build.yml`) build tự động và upload file `.bin`
(có cả `matter_relay_merged.bin` để nạp một lần ở địa chỉ `0x0`).

## Commissioning

Firmware dùng chứng chỉ test (DAC test) của esp-matter, mã ghép nối mặc định:

- Manual code: **34970112332**
- QR: `MT:Y.K9042C00KA0648G00`
- Discriminator 3840, passcode 20202021

Mở Apple Home / Google Home / SmartThings / Home Assistant → *Add Matter device* → quét QR
hoặc nhập mã ở trên. Với chip-tool:

```bash
chip-tool pairing ble-wifi 0x7283 <SSID> <PASSWORD> 20202021 3840
chip-tool onoff toggle 0x7283 1
```

Vì là thiết bị dùng chứng chỉ test, app sẽ cảnh báo "Uncertified accessory" – chọn tiếp tục.

## Cấu trúc

```
CMakeLists.txt            project esp-matter (esp32c3)
sdkconfig.defaults        cấu hình chung Matter/Wi-Fi/BLE
sdkconfig.defaults.esp32c3 chân nút, relay
partitions.csv            4 MB, 2 phân vùng OTA
main/app_main.cpp         tạo node + endpoint On/Off Plug-in Unit
main/app_driver.cpp       điều khiển GPIO relay + nút
main/Kconfig.projbuild    tuỳ chọn RELAY_GPIO / RELAY_ACTIVE_LEVEL
```

## Sơ đồ đấu nối

```
ESP32-C3 GPIO5 ──► IN  (module relay 3.3V hoặc qua transistor)
ESP32-C3 3V3   ──► VCC (module relay 5V: cấp 5V, IN vẫn dùng mức 3.3V)
ESP32-C3 GND   ──► GND
```

⚠️ Phía tải dùng điện 220V: cách ly và đi dây an toàn.
