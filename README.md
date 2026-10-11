# ESP32-C3 Matter over Wi-Fi – Relay GPIO5 + đo công suất BL0937

Firmware ESP-IDF + [esp-matter](https://github.com/espressif/esp-matter) cho ESP32-C3, tạo thiết bị Matter
**On/Off Plug-in Unit** (ổ cắm/công tắc) điều khiển **relay ở GPIO5**, kết nối qua **Wi-Fi**,
commissioning qua **BLE**.

| Chức năng | Chân |
|---|---|
| Relay (active-high mặc định) | GPIO5 |
| Nút BOOT – nhấn ngắn: bật/tắt relay | GPIO9 |
| Nút BOOT – giữ 5 s rồi thả: factory reset | GPIO9 |
| BL0937 CF (xung công suất) | GPIO6 |
| BL0937 CF1 (xung dòng / áp) | GPIO7 |
| BL0937 SEL (chọn dòng / áp cho CF1) | GPIO10 |

Trạng thái relay được lưu NVS và khôi phục sau khi mất điện.

## Đo công suất (BL0937)

Cùng endpoint với relay được thêm device type **Electrical Sensor** của Matter:

| Cluster | Thuộc tính | Đơn vị |
|---|---|---|
| Electrical Power Measurement | ActivePower | mW |
| | Voltage | mV |
| | ActiveCurrent | mA |
| Electrical Energy Measurement | CumulativeEnergyImported | mWh (lưu NVS mỗi 10 phút) |

- Cập nhật mỗi 2 s (`BL0937_UPDATE_INTERVAL_MS`); CF1 luân phiên đo áp / dòng giữa các cửa sổ.
- ESP32-C3 không có bộ đếm xung PCNT nên xung được đếm bằng ngắt GPIO và đo chu kỳ, vẫn chính xác ở tải nhỏ.
- Tải quá nhỏ để đo dòng trên CF1 → dòng được tính bằng P/U.

**Hiển thị trên app:** dữ liệu theo chuẩn Matter 1.3+ nên Home Assistant, SmartThings, Google Home đọc được.
Apple Home có hiển thị công suất hay không tuỳ phiên bản iOS – nếu không thấy trong Apple Home, dữ liệu vẫn có
trên Matter và đọc được qua Home Assistant hoặc chip-tool:

```bash
chip-tool electricalpowermeasurement read active-power 0x7283 1
chip-tool electricalenergymeasurement read cumulative-energy-imported 0x7283 1
```

### Hiệu chuẩn

`idf.py menuconfig` → **BL0937 Power Meter**:

| Tuỳ chọn | Mặc định | Ý nghĩa |
|---|---|---|
| `BL0937_SHUNT_MICROOHM` | 1000 | điện trở shunt 1 mΩ |
| `BL0937_VOLTAGE_DIVIDER` | 2351 | phân áp (5 × 470 kΩ + 1 kΩ) / 1 kΩ |
| `BL0937_SEL_CURRENT_LEVEL` | 0 | mức SEL để CF1 ra dòng (đổi nếu áp và dòng bị tráo) |
| `BL0937_*_CAL_PERMILLE` | 1000 | hệ số hiệu chỉnh ‰ cho công suất / áp / dòng |

Cách hiệu chỉnh: cắm tải thuần trở đã biết (bóng sợi đốt / ấm đun 1000 W), so với đồng hồ chuẩn,
rồi đặt `CAL = 1000 × giá_trị_chuẩn / giá_trị_đọc`. Ví dụ đọc 1050 W, thực tế 1000 W → `POWER_CAL_PERMILLE=952`.

### Lưu ý phần cứng

- BL0937 thường nối GND về dây pha/trung tính của lưới điện → **mạch không cách ly**. Không cắm USB vào máy tính
  khi mạch đang cấp 220 V.
- Nếu BL0937 cấp 5 V, chân CF/CF1 ra mức 5 V: cần phân áp hoặc dịch mức về 3.3 V trước khi vào ESP32-C3.

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
main/bl0937.cpp           driver BL0937 (đếm xung CF / CF1 bằng ngắt GPIO)
main/power_meter.cpp      Electrical Sensor: đưa công suất / áp / dòng / điện năng lên Matter
main/Kconfig.projbuild    tuỳ chọn RELAY_GPIO / RELAY_ACTIVE_LEVEL
```

## Sơ đồ đấu nối

```
ESP32-C3 GPIO5 ──► IN  (module relay 3.3V hoặc qua transistor)
ESP32-C3 3V3   ──► VCC (module relay 5V: cấp 5V, IN vẫn dùng mức 3.3V)
ESP32-C3 GND   ──► GND
```

⚠️ Phía tải dùng điện 220V: cách ly và đi dây an toàn.
