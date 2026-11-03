# Bài 3 — Điều khiển thiết bị an toàn

**Học phần:** CSE3069 — Phát triển ứng dụng IoT  
**Họ tên:** Nguyễn Trung Hà  
**MSSV:** 23110323  

## 1. Mục tiêu

Bài thực hành xây dựng chương trình điều khiển tải trên mô hình nhà kính bằng ESP32 theo hướng an toàn. Hệ thống sử dụng ba trạng thái **DỪNG / CHẠY / LỖI**, có giới hạn thời gian chạy, PWM cho tải, đọc dữ liệu cảm biến qua I2C và sử dụng timer phần cứng.

Firmware hiện tại điều khiển ba kênh:

- LED1
- LED2
- MOTOR1

ESP32 đóng vai trò I2C master. Arduino UNO đọc cảm biến và trả về 4 byte dữ liệu cho ESP32 theo chu kỳ 2 giây.

Quy ước giải mã đang sử dụng:

- Byte 0–1: nhiệt độ × 10
- Byte 2–3: độ ẩm × 10

Nhiệt độ và độ ẩm chỉ được xem là hợp lệ khi nhận đủ gói dữ liệu và nằm trong giới hạn kiểm tra của chương trình.

## 2. Điều khiển tải

### LED1 và LED2

Hai LED sử dụng PWM active-HIGH:

- LED1: GPIO13
- LED2: GPIO12
- Tần số PWM: 1 kHz
- Độ phân giải: 8 bit
- Các mức thử: 25%, 50%, 90%

Khi hệ thống ở trạng thái DỪNG hoặc LỖI, duty cycle được đưa về 0.

### MOTOR1

MOTOR1 sử dụng:

- IN1: GPIO5 — PWM
- IN2: GPIO17 — giữ LOW
- Tần số PWM: 1 kHz
- Các mức duty: 25%, 50%, 75%, 100%

Mức mặc định khi bật MOTOR1 là 100%.

Nếu xảy ra lỗi PWM, chương trình đưa chân điều khiển về LOW và không tiếp tục cho phép kênh đó chạy.

## 4. Máy trạng thái

Mỗi kênh hoạt động theo ba trạng thái:

| Trạng thái | Ý nghĩa | Đầu ra |
| --- | --- | --- |
| DỪNG | Trạng thái an toàn | Tải tắt |
| CHẠY | Tải đang hoạt động | PWM theo duty đã chọn |
| LỖI | Có lỗi cảm biến, timeout hoặc lệnh không hợp lệ | Tải tắt |

Giới hạn mỗi lần chạy là **20 giây**.

Luồng chính:

```text
DỪNG
  |
  | START + dữ liệu hợp lệ
  v
CHẠY
  |
  | STOP
  +----------------------> DỪNG
  |
  | Hết 20 giây / dữ liệu lỗi
  v
LỖI
  |
  v
DỪNG
```


## 5. Timer phần cứng

ESP32 sử dụng hardware timer tạo sự kiện định kỳ mỗi **1 giây**.

ISR timer chỉ thực hiện công việc ngắn:

- vào critical section;
- tăng biến đếm `timerTicks`;
- thoát critical section.

Việc in Serial và xử lý kết quả được thực hiện bên ngoài ISR.

Cách này giúp ISR không bị kéo dài bởi thao tác Serial hoặc các xử lý khác.

## 6. Đọc cảm biến qua I2C

ESP32 yêu cầu 4 byte từ UNO tại địa chỉ `0x08` mỗi 2 giây.

Gói dữ liệu được giải mã thành:

```text
temperature = ((byte0 << 8) | byte1) / 10.0
humidity    = ((byte2 << 8) | byte3) / 10.0
```

Chương trình đánh dấu dữ liệu không hợp lệ trong các trường hợp:

- không nhận đủ 4 byte;
- I2C chưa khởi tạo thành công;
- chưa từng có mẫu hợp lệ;
- dữ liệu gần nhất đã quá cũ;
- nhiệt độ hoặc độ ẩm vượt giới hạn kiểm tra.

Khi dữ liệu đầu vào không hợp lệ trong lúc tải đang chạy, FSM đưa kênh về trạng thái lỗi và tắt tải.

## 7. Giao diện điều khiển

ESP32 tạo Wi-Fi Access Point:

```text
SSID: TH03-Greenhouse
Password: th03demo123
```

Sau khi kết nối vào mạng của ESP32, địa chỉ trang điều khiển được in trên Serial Monitor.

Giao diện web cho phép:

- xem nhiệt độ và độ ẩm;
- xem trạng thái LED1, LED2 và MOTOR1;
- bật/tắt từng kênh;
- thay đổi PWM;
- xem thời gian chạy còn lại;
- dừng toàn bộ tải.

Trang web chỉ gửi yêu cầu điều khiển. Việc quyết định tải có được chạy hay không vẫn do FSM trên ESP32 xử lý.

## 8. Các ca kiểm thử bắt buộc

### Ca 1 — Bật/tắt bình thường

**Thao tác:** Khi dữ liệu cảm biến hợp lệ, bật tải và tắt trước khi đạt giới hạn 20 giây.

**Kỳ vọng:**  
`DỪNG → CHẠY → DỪNG`

### Ca 2 — Hết thời gian

**Thao tác:** Bật tải và không gửi lệnh tắt.

**Kỳ vọng:** Sau 20 giây tải tự tắt và hệ thống quay lại trạng thái an toàn.

### Ca 3 — Dữ liệu lỗi

**Thao tác:** Tạo dữ liệu cảm biến không hợp lệ khi tải đang chạy.

**Kỳ vọng:**  
`CHẠY → LỖI → DỪNG`, tải bị tắt.


