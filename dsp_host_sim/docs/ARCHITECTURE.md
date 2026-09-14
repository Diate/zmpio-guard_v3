# dsp_host_sim — Kiến trúc & thiết kế DSP (gate Step1)

Tài liệu này mô tả **thiết kế hiện hành** của `dsp_host_sim/`: contract input/output, chuỗi xử lý
DSP fixed-point, streaming/back-pressure contract và cách các thành phần này bám baseline
CPU1/MPU6050 thật. Chi tiết build, verification, dataset và điều kiện bàn giao sang Step2 nằm ở
[`IMPLEMENTATION_AND_DEPLOYMENT.md`](IMPLEMENTATION_AND_DEPLOYMENT.md).

`dsp_host_sim/` là snapshot độc lập ứng với gate **Step1** trong `STEP_WORKSPACE.md` — nó chứng
minh chuỗi xử lý tín hiệu của ZMPIO Guard V3 trên host trước khi thay đổi Vivado block design,
firmware CPU1, ABI IPC hoặc Linux. Kết quả của step này phải trả lời được bốn câu hỏi:

1. Dữ liệu MPU6050 hiện tại có một contract đầu vào rõ ràng hay chưa?
2. FIR, FFT và feature extraction fixed-point có hoạt động xác định và lặp lại được hay không?
3. Output feature có ABI cố định để Step2 tích hợp vào PL/CPU1 hay chưa?
4. Source, config, test và golden vector có thể build lại mà không phụ thuộc vào việc chỉnh sửa
   baseline đang chạy hay không?

`dsp_host_sim/` không có nhiệm vụ tích hợp FPGA trên board. Việc tạo AXI bridge, IRQ, BRAM FIFO,
XSA và firmware HAL thuộc Step2 (`hardware/rtl/`).

Toàn bộ source của snapshot này nằm dưới `zmpio-guard/dsp_host_sim/`. Không có file nào trong các
thư mục baseline sau bị sửa bởi snapshot này: `common/`, `software/linux_ipc/`,
`deploy/petalinux_overlay/`, `firmware/`. Step0 tiếp tục dùng code baseline hiện hành; khi phát
triển Step2 trở đi, `dsp_host_sim/` vẫn phải giữ nguyên để có thể quay lại build và so sánh golden
vector — quy ước tổng quát nằm trong `STEP_WORKSPACE.md` ở root dự án.

## 1. Baseline & traceability

Các file baseline dùng để suy ra contract được ghi cùng SHA256 trong
`config/upstream_manifest.json`. Chạy `VERIFY_UPSTREAM.cmd` để phát hiện một file upstream đã bị
thay đổi kể từ lúc snapshot này được tạo.

### 1.1 Dữ liệu MPU6050

`common/zmpio_protocol.h` xác nhận một mẫu cảm biến hiện có gồm bảy giá trị signed 16-bit theo thứ
tự: accelerometer X, accelerometer Y, accelerometer Z, temperature, gyroscope X, gyroscope Y,
gyroscope Z.

`app_config.h` và `mpu6050.c` xác nhận cấu hình baseline:

- sample rate: 100 Hz, tương đương một mẫu mỗi 10 ms;
- accelerometer: ±8 g, 4096 raw count/g;
- gyroscope: ±500 degree/s, 65.5 raw count/(degree/s);
- MPU6050 DLPF: 21 Hz;
- `SMPLRT_DIV=9` từ internal rate 1 kHz.

Do DLPF đầu vào là 21 Hz, dải feature V3 hiện được khóa tại 0–20 Hz. Các bin trên 20 Hz không được
dùng làm feature ML.

### 1.2 ZLOG

Baseline dùng ZLOG version 1: file header 32 byte, record header 28 byte, payload tối đa 240 byte,
CRC32 cho từng payload, source MPU6050 có payload đúng 14 byte (`7 × int16`).
`tools/zlog_to_raw_csv.py` triển khai lại contract này trong snapshot. Script kiểm tra magic,
version, size và CRC trước khi xuất dữ liệu cảm biến.

### 1.3 Bảng traceability

Snapshot này cố ý tự chứa (self-contained) để một checkout Step2/Step3 sau này có thể chuyển đổi
mà không ghi đè lên baseline đang chạy.

| Khu vực đã kiểm tra | Fact tái sử dụng | Hành động trong snapshot |
|---|---|---|
| `common/zmpio_protocol.h` | MPU sample là 7 × signed 16-bit | Chốt trong `sample_frame_v1.h`, pin trong upstream manifest |
| `firmware/app_freertos/src/app_config.h` | 100 Hz, ±8 g, DLPF 21 Hz | Khóa trong DSP contract |
| `firmware/app_freertos/src/mpu6050.c` | Register config `0x04/9/0x08/0x10` | Khóa trong DSP contract; baseline source không đổi |
| `firmware/app_freertos/src/log_record.h` | ZLOG v1 framing và CRC | Triển khai lại bởi ZLOG converter của snapshot |
| `software/linux_ipc/parse_zlog.py` | ZLOG parsing semantics hiện có | Dùng làm tham chiếu; baseline parser không đổi |
| `deploy/petalinux_overlay/` | Boot/package inputs và CPU1 ELF | Không thuộc phạm vi snapshot; không đổi |

Không có file nào dưới `software/linux_ipc/`, `deploy/petalinux_overlay/` hay `firmware/` bị sửa
bởi snapshot này. Tích hợp bắt đầu ở Step2 và phải dùng lại header public từ snapshot này hoặc
copy chúng vào snapshot Step2 riêng.

## 2. Input contract — `SampleFrameV1`

`SampleFrameV1` nằm trong `fpga/dsp_core/src/sample_frame_v1.h` và có kích thước chính xác 20
byte, tương đương AXIS `tdata=160` dự kiến cho Step2.

| Offset | Field | Kiểu | Mục đích |
|---:|---|---|---|
| 0 | `accel_x` | `int16` | Raw accelerometer X |
| 2 | `accel_y` | `int16` | Raw accelerometer Y |
| 4 | `accel_z` | `int16` | Raw accelerometer Z |
| 6 | `temperature` | `int16` | Raw temperature |
| 8 | `gyro_x` | `int16` | Raw gyroscope X |
| 10 | `gyro_y` | `int16` | Raw gyroscope Y |
| 12 | `gyro_z` | `int16` | Raw gyroscope Z |
| 14 | `flags` | `uint16` | Cờ validity/error dành cho Step2 |
| 16 | `sample_sequence` | `uint32` | Sequence tăng theo mẫu được chấp nhận |

Chỉ ba kênh accelerometer và sequence được dùng cho DSP. Temperature, gyroscope và flags vẫn nằm
trong frame để Step2 không phải thay đổi payload 160-bit.

Contract hiện giả định little-endian trên ARM target. Step2 phải ghi năm word MMIO theo layout này
rồi mới phát thành AXI4-Stream; CPU1 không có AXIS master trực tiếp.

### Ví dụ đóng gói một `SampleFrameV1`

Giả sử một mẫu có giá trị: `accel_x=1000` (`0x03E8`), `accel_y=-200` (`0xFF38`), `accel_z=4096`
(`0x1000`), `temperature=250` (`0x00FA`), `gyro_x=-50` (`0xFFCE`), `gyro_y=25` (`0x0019`),
`gyro_z=0` (`0x0000`), `flags=3` (`0x0003`), `sample_sequence=4660` (`0x00001234`).

Vì little-endian, payload 20 byte theo thứ tự offset là:

```text
E8 03 38 FF 00 10 FA 00 CE FF 19 00 00 00 03 00 34 12 00 00
```

4 byte cuối `34 12 00 00` chính là `sample_sequence` tăng đơn điệu theo mỗi lần handshake input
thành công.

## 3. Chuỗi xử lý DSP

```text
SampleFrameV1
     │ accel X/Y/Z
     ▼
FIR 16 tap Q1.15 × 3 trục
     │ filtered X/Y/Z
     ▼
integer sqrt(x²+y²+z²)
     │ vector magnitude
     ▼
ring window 128, hop 64
     ├── detrend ── RMS / peak / variance / kurtosis
     └── detrend + Hann Q1.15 ── FFT-128 ── spectral features
                                           │
                                           ▼
                                  FeatureFrameV2 (48 B)
```

Ở 100 Hz: thời gian thu một cửa sổ 1.28 giây; chu kỳ feature sau cửa sổ đầu tiên 640 ms; độ phân
giải FFT `100/128 = 0.78125 Hz/bin`; phổ one-sided đầy đủ bin 0–64; phổ dùng cho V3 là bin 1–25.

### 3.1 FIR ba trục

`Fir3Axis` chứa ba history buffer độc lập, mỗi buffer 16 mẫu. Ba bộ lọc FIR đối xứng, độc lập, 16
tap, hệ số Q1.15 dùng chung cho cả ba trục.

Coefficient version 1:

```text
-90, 82, 427, -58, -1742, -995, 5570, 13190,
13190, 5570, -995, -1742, -58, 427, 82, -90
```

Tổng coefficient bằng 32768, vì vậy DC gain bằng 1. Accumulator dùng 64-bit; kết quả được round
nearest, ties away from zero, dịch phải 15 bit và saturate về signed 16-bit.

Mục tiêu response được test: suy hao nhỏ hơn 1 dB trong 0–15 Hz (passband); suy hao lớn hơn 30 dB
trong 35–50 Hz (stopband); vùng giữa 15–35 Hz là transition band. Coefficient và target nằm trong
`fpga/dsp_core/coefficients/coefficients_v1.json`.

Công thức cho một trục:

```text
acc[n] = Σ(h[k] * x[n-k]), k=0..15
y[n]   = sat16(round_ties_away(acc[n] / 2^15))

if acc >= 0: q = (acc + 16384) >> 15
if acc <  0: q = -(((-acc) + 16384) >> 15)
```

Ví dụ DC gain unity với input hằng `x[n-k]=1000` cho mọi `k`: `Σh = 32768`,
`acc = 1000 × 32768 = 32,768,000`, `y = round(32,768,000 / 32768) = 1000` — không đổi biên độ DC,
đúng với mục tiêu tổng hệ số bằng 1.0 (Q1.15).

### 3.2 Vector magnitude và detrend

Sau FIR: `magnitude = integer_sqrt(x² + y² + z²)`. Magnitude được lưu trong ring 128 phần tử.
Trước khi tính feature, trung bình cửa sổ được trừ khỏi từng mẫu (detrend) để loại thành phần DC
chủ yếu do trọng lực và giúp FFT tập trung vào độ rung biến thiên.

Ví dụ: sau FIR tại một thời điểm `x=300, y=400, z=1200` →
`x² + y² + z² = 1,690,000` → `magnitude = integer_sqrt(1,690,000) = 1300`.

### 3.3 Window Hann

FFT input được nhân với Hann window 128 điểm Q1.15 (`hann128_q15.h`, version hóa, runtime không
gọi `sin`/`cos` hay dùng floating-point). RMS, peak, variance và kurtosis được tính trên magnitude
đã detrend nhưng **chưa** nhân Hann — Hann chỉ dùng cho nhánh phổ.

Công thức nhân cửa sổ: `xw[n] = round_ties_away((d[n] * hann_q15[n]) / 2^15)`. Ở biên cửa sổ,
`hann_q15[0]=0` và `hann_q15[127]=0`, nên mẫu biên sau nhân window bằng 0.

### 3.4 FFT-128 fixed-point

`fft128_real.cpp` triển khai FFT radix-2 decimation-in-time: bit-reversal 7 bit, bảy butterfly
stage, twiddle Q1.15 lưu tĩnh trong `fft_twiddles_q15.h`, mỗi stage dịch phải một bit sau
butterfly, total scale `1/128`, phép nhân twiddle dùng accumulator 64-bit và rounding xác định.
Scale schedule nằm trong `fft_config_v1.json` — Step2/HLS không được tự thay scale schedule vì mọi
golden power và ML normalization phụ thuộc vào nó.

Ánh xạ tần số theo bin: `f[k] = k * Fs / N = k * 100 / 128 = k * 0.78125 (Hz)`.

### 3.5 Feature miền thời gian

Mười feature đầu ra: RMS của magnitude đã detrend, absolute peak, variance, non-excess kurtosis,
dominant frequency, dominant power, và 4 band energy. Kurtosis dùng phép tính integer — dữ liệu
detrend được scale động để trị tuyệt đối không vượt 127 trước khi tính moment bậc hai/bốn, giữ
phép nhân trong miền 64-bit. Cửa sổ phẳng có variance bằng zero sẽ trả kurtosis bằng zero.

### 3.6 Feature miền tần số

Power một bin là `real² + imag²` sau FFT scaling và saturate về `uint32`.

| Band | Bin | Dải tần xấp xỉ |
|---:|---:|---:|
| 0 | 1–6 | 0.78125–4.6875 Hz |
| 1 | 7–12 | 5.46875–9.375 Hz |
| 2 | 13–18 | 10.15625–14.0625 Hz |
| 3 | 19–25 | 14.84375–19.53125 Hz |

DC và bin 26–64 không được cộng vào band energy. Dominant frequency chỉ được tìm trong bin 1–25.
Nếu tất cả bin có power bằng zero thì dominant frequency và dominant power đều phải bằng zero —
implementation không được báo bin 1 như một tone thực.

## 4. Output contract — `FeatureFrameV2`

`FeatureFrameV2` nằm trong `feature_frame_v2.h`, có kích thước compile-time chính xác 48 byte.

| Offset | Field | Q format/đơn vị |
|---:|---|---|
| 0 | `frame_sequence` | `uint32`, tăng theo feature frame |
| 4 | `window_end_sample_sequence` | `uint32`, sequence mẫu cuối cửa sổ |
| 8 | `rms_q24_8` | unsigned Q24.8 raw count |
| 12 | `peak_q24_8` | unsigned Q24.8 raw count |
| 16 | `variance_q32_0` | raw count² integer |
| 20 | `kurtosis_q16_16` | unsigned Q16.16 |
| 24 | `dominant_frequency_q16_16` | unsigned Q16.16 Hz |
| 28 | `dominant_power_q32_0` | scaled FFT power |
| 32 | `band_energy_q32_0[4]` | bốn tổng power saturating |

Timestamp không nằm trong frame 48 byte. CPU1 Step2/Step3 phải đặt timestamp, record type, flags và
CRC trong metadata ZLOG/IPC bao ngoài. ABI dùng `static_assert` cho size, offset quan trọng,
standard layout và trivially-copyable. Step2 phải copy nguyên header này hoặc pin đúng hash; không
được khai báo lại struct bằng tay.

Làm tròn là gần nhất, với tie tách xa khỏi zero cho các phép toán Q1.15 có dấu. Tràn ở một trường
ABI sẽ bão hòa; nó không bao giờ quấn vòng âm thầm.

## 5. Back-pressure / streaming contract

`DspCore` mô phỏng contract streaming tối thiểu:

- input chỉ được nhận khi `input_ready()` trả true;
- khi hoàn tất một feature frame, core giữ frame trong `pending_`;
- trong lúc output chưa được `pop()`, core deassert input-ready;
- sequence input chỉ tăng sau một handshake thành công;
- output không được ghi đè khi consumer đang back-pressure.

Snapshot này chỉ có một pending register, không phải FIFO production. Step2 phải thêm FIFO ở shell
để CPU1 không bị stall ngoài ý muốn, nhưng không được thay đổi thứ tự hoặc nội dung frame.

Ví dụ handshake: tại `t0` core vừa hoàn tất một feature frame → `pending_` đầy, `output_valid=1`,
`input_ready=0`. Tại `t1` producer có sample mới nhưng không handshake được. Tại `t2` consumer
assert ready và gọi `pop()`. Tại `t3` `pending_` rỗng, core trả `input_ready=1`, sample kế tiếp mới
được nhận. Hệ quả kiểm chứng được bằng sequence: trong thời gian back-pressure sequence không tăng;
sample tiếp theo được nhận thành công mới có sequence kế tiếp — không mất mẫu, không nhảy thứ tự
và không ghi đè output đang chờ đọc.

## 6. Phân tích an toàn số học

- FIR: `int16 × int16 × 16 tap` nằm an toàn trong accumulator signed 64-bit.
- Vector magnitude: tổng ba bình phương `int16` nằm trong unsigned 64-bit.
- Variance: tối đa 128 bình phương magnitude vẫn nằm trong unsigned 64-bit.
- Kurtosis: input moment được dịch để trị tuyệt đối tối đa 127, giữ numerator và denominator trong
  unsigned 64-bit.
- FFT butterfly: giá trị được scale 1 bit mỗi stage; tích twiddle dùng signed 64-bit.
- Mọi field ABI 32-bit có thể vượt miền đều dùng saturating conversion.

UBSan chưa được tính là PASS: GCC 6.3 local gặp internal compiler error khi bật
`-fsanitize=undefined`. Nên chạy sanitizer bằng GCC/Clang mới hơn trước khi coi snapshot này là
"hoàn toàn đã kiểm chứng số học runtime".
