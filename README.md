# ESP32-C3 Matter – Máy lọc không khí (relay nút nguồn + đo công suất BL0937)

Firmware ESP-IDF + [esp-matter](https://github.com/espressif/esp-matter) cho ESP32-C3, biến một máy lọc
không khí thường thành thiết bị Matter **Air Purifier** (hiện trong Apple Home / HomeKit), kết nối qua **Wi-Fi**,
commissioning qua **BLE**.

- Relay có 2 tiếp điểm **đấu song song với 2 chân nút nguồn** của máy lọc. Bật hoặc tắt trên HomeKit →
  relay đóng **0,3 s** rồi nhả, giống như bấm nút nguồn.
- Trạng thái máy **không đoán theo lệnh** mà lấy từ công suất đo được bằng BL0937:
  **P > 3 W → ON**, ngược lại **OFF**. Bật/tắt bằng nút trên máy hay remote cũng được cập nhật lên HomeKit.
- Chỉ có bật / tắt như công tắc, vẫn hiển thị là **máy lọc không khí**: Fan Control dùng
  `FanModeSequence = Off/High` và chỉ **1 cấp tốc độ** (`SpeedMax = 1`). Chuẩn Matter bắt buộc có thuộc tính
  phần trăm tốc độ nên Apple Home có thể vẫn vẽ thanh trượt trong màn hình chi tiết, nhưng nó chỉ có 2 vị trí
  tắt / bật; kéo tới bất kỳ mức nào khác 0 cũng chỉ là "bật" và firmware kéo thanh về 100 %.

| Chức năng | Chân |
|---|---|
| Relay "bấm nút nguồn" (active-high mặc định) | GPIO5 |
| Nút BOOT – nhấn ngắn: bật/tắt máy lọc | GPIO9 |
| Nút BOOT – giữ 5 s rồi thả: factory reset | GPIO9 |
| BL0937 CF (xung công suất) | GPIO6 |
| BL0937 CF1 (xung dòng / áp) | GPIO7 |
| BL0937 SEL (chọn dòng / áp cho CF1) | GPIO10 |

## Cách hoạt động

```
HomeKit bật/tắt ──► Fan Control (FanMode / PercentSetting) ──► purifier task
                                                                 │ trạng thái yêu cầu ≠ trạng thái thực?
                                                                 ▼
                                                     relay đóng 0,3 s (bấm nút nguồn)
BL0937 ── P mỗi 1 s ──► P > 3 W (2 lần liên tiếp) ? ON : OFF ──► FanMode + PercentCurrent ──► HomeKit
```

1. Lệnh từ HomeKit chỉ "bấm nút" khi trạng thái yêu cầu khác trạng thái hiện tại – vì nút nguồn là nút
   toggle, bấm thừa sẽ làm máy đảo ngược trạng thái.
2. Sau khi bấm, HomeKit hiển thị trạng thái vừa yêu cầu trong tối đa 8 s chờ công suất xác nhận.
   Máy không phản ứng (mất điện, rút phích…) → quay về báo trạng thái đo được.
3. Ngoài khoảng chờ đó, HomeKit luôn hiển thị trạng thái đo được.
4. Khi khởi động, firmware **không bấm nút**: chỉ đọc công suất rồi báo trạng thái lên HomeKit.

Logic nằm trong `main/purifier.cpp`. Lệnh từ Matter đến qua `FanControl::Delegate::OnFanDriveStateChanged`
(trong esp-matter v1.6, Fan Control là cluster code-driven nên không đi qua attribute callback của ứng dụng).

### Tuỳ chỉnh

`idf.py menuconfig` → **Air Purifier**:

| Tuỳ chọn | Mặc định | Ý nghĩa |
|---|---|---|
| `RELAY_GPIO` | 5 | chân điều khiển relay |
| `RELAY_ACTIVE_LEVEL` | 1 | 1 = active-high, 0 = active-low (module relay opto rẻ tiền thường là active-low) |
| `PURIFIER_PULSE_MS` | 300 | thời gian đóng relay = thời gian "bấm" nút |
| `PURIFIER_PULSE_GAP_MS` | 700 | khoảng nghỉ tối thiểu giữa 2 lần bấm |
| `PURIFIER_ON_THRESHOLD_MW` | 3000 | ngưỡng công suất: lớn hơn → ON |
| `PURIFIER_DEBOUNCE_SAMPLES` | 2 | số lần đo liên tiếp (mỗi lần 1 s) để đổi trạng thái |
| `PURIFIER_CONFIRM_TIMEOUT_MS` | 8000 | thời gian chờ máy phản ứng sau khi bấm |

Hoặc sửa `sdkconfig.defaults.esp32c3` rồi xoá `sdkconfig` để áp dụng lại.

Build sẽ báo lỗi ngay nếu cấu hình không hợp lệ: chân relay / BL0937 trùng nhau, trùng GPIO9 (nút BOOT) hoặc
GPIO11–17 (flash), hay `PURIFIER_CONFIRM_TIMEOUT_MS` ngắn hơn thời gian cần để công suất xác nhận trạng thái.

## Đo công suất (BL0937)

BL0937 luôn được dùng để biết máy đang bật hay tắt. Ngoài ra (tuỳ chọn `POWER_METER_MATTER`, mặc định bật)
endpoint máy lọc được thêm device type **Electrical Sensor** của Matter:

| Cluster | Thuộc tính | Đơn vị |
|---|---|---|
| Electrical Power Measurement | ActivePower | mW |
| | Voltage | mV |
| | ActiveCurrent | mA |
| Electrical Energy Measurement | CumulativeEnergyImported | mWh (lưu NVS mỗi 10 phút) |

- Cập nhật mỗi 1 s (`BL0937_UPDATE_INTERVAL_MS`); CF1 luân phiên đo áp / dòng giữa các cửa sổ.
- ESP32-C3 không có bộ đếm xung PCNT nên xung được đếm bằng ngắt GPIO và đo chu kỳ, vẫn chính xác ở tải nhỏ.
- Tải quá nhỏ để đo dòng trên CF1 → dòng được tính bằng P/U.

**Hiển thị trên app:** dữ liệu theo chuẩn Matter 1.3+ nên Home Assistant, SmartThings, Google Home đọc được.
Apple Home có hiển thị công suất hay không tuỳ phiên bản iOS – nếu không thấy trong Apple Home, dữ liệu vẫn có
trên Matter và đọc được qua Home Assistant hoặc chip-tool (nếu Apple Home hiển thị máy lọc không đúng vì
endpoint có thêm Electrical Sensor, tắt `POWER_METER_MATTER` trong menuconfig):

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
(có cả `matter_air_purifier_merged.bin` để nạp một lần ở địa chỉ `0x0`).

## Commissioning

Firmware dùng chứng chỉ test (DAC test) của esp-matter, mã ghép nối mặc định:

- Manual code: **34970112332**
- QR: `MT:Y.K9042C00KA0648G00`
- Discriminator 3840, passcode 20202021

Mở Apple Home / Google Home / SmartThings / Home Assistant → *Add Matter device* → quét QR
hoặc nhập mã ở trên. Với chip-tool:

```bash
chip-tool pairing ble-wifi 0x7283 <SSID> <PASSWORD> 20202021 3840
chip-tool fancontrol write fan-mode 3 0x7283 1   # bật (High)
chip-tool fancontrol write fan-mode 0 0x7283 1   # tắt
chip-tool fancontrol read percent-current 0x7283 1 # 100 = máy đang chạy (P > 3 W)
```

Vì là thiết bị dùng chứng chỉ test, app sẽ cảnh báo "Uncertified accessory" – chọn tiếp tục.

Nếu mạch trước đó đã chạy firmware ổ cắm (On/Off Plug-in Unit) và đã ghép vào Home: xoá thiết bị khỏi Home,
nạp lại với `erase-flash`, rồi ghép lại để HomeKit nhận đúng loại thiết bị máy lọc không khí.

## Cấu trúc

```
CMakeLists.txt             project esp-matter (esp32c3)
sdkconfig.defaults         cấu hình chung Matter/Wi-Fi/BLE, bật Fan Control cluster
sdkconfig.defaults.esp32c3 chân nút, relay, BL0937
partitions.csv             4 MB, 2 phân vùng OTA
main/app_main.cpp          tạo node + endpoint Air Purifier
main/app_driver.cpp        nút BOOT
main/purifier.cpp          relay bấm nút nguồn + đồng bộ trạng thái theo công suất
main/bl0937.cpp            driver BL0937 (đếm xung CF / CF1 bằng ngắt GPIO)
main/power_meter.cpp       đọc BL0937 định kỳ, đưa công suất / áp / dòng / điện năng lên Matter
main/Kconfig.projbuild     tuỳ chọn Air Purifier / BL0937
```

## Sơ đồ đấu nối

```
ESP32-C3 GPIO5 ──► IN  (module relay 3.3V hoặc qua transistor)
ESP32-C3 3V3   ──► VCC (module relay 5V: cấp 5V, IN vẫn dùng mức 3.3V)
ESP32-C3 GND   ──► GND

Relay COM ──┬── chân 1 nút nguồn máy lọc
Relay NO  ──┴── chân 2 nút nguồn máy lọc     (song song với nút, dùng tiếp điểm thường mở NO)

Phích máy lọc ──► mạch BL0937 (đo công suất máy lọc) ──► 220 V
```

- Dùng tiếp điểm **NO** (thường mở): mất điện ESP32 thì nút trên máy vẫn bấm được bình thường.
- Nút nguồn trên mạch máy lọc thường là mạch tín hiệu điện áp thấp, nhưng **có máy mạch nút nối thẳng lưới 220 V
  (nguồn không cách ly)** – kiểm tra trước, relay có tiếp điểm cách ly nên vẫn dùng được, chỉ cần đi dây an toàn.
- Relay không được đóng khi ESP32 khởi động (nếu không máy sẽ bị bật/tắt mỗi lần reset). Module relay active-high
  qua transistor có điện trở kéo xuống là an toàn; nếu thấy relay "tạch" lúc cấp điện, đổi sang chân GPIO khác
  hoặc thêm điện trở kéo về mức không kích.

⚠️ Phía tải dùng điện 220V: cách ly và đi dây an toàn.
