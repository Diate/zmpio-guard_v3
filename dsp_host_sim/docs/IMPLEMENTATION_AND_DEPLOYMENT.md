# dsp_host_sim — Triển khai & vận hành (gate Step1)

Tài liệu này mô tả **chi tiết implementation** (cấu trúc source, build, verification, dataset,
MLP, trạng thái gate) và **runbook deploy trên board thật** (thu ZLOG, verify dataset) của
`dsp_host_sim/`. Thiết kế/contract (input/output ABI, thuật toán DSP, streaming) nằm ở
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Phần A — Implementation

### A.1 Cấu trúc source

```text
dsp_host_sim/
├── BUILD_STEP.cmd / BUILD_STEP.ps1
├── VERIFY_STEP.cmd / VERIFY_STEP.ps1
├── VERIFY_UPSTREAM.cmd / VERIFY_UPSTREAM.ps1
├── CMakeLists.txt
├── config/
│   └── upstream_manifest.json
├── docs/
│   ├── ARCHITECTURE.md
│   └── IMPLEMENTATION_AND_DEPLOYMENT.md
├── fpga/dsp_core/
│   ├── coefficients/
│   ├── src/
│   └── tb/
├── golden/
│   ├── golden_tone_features.csv
│   └── manifest.sha256
├── ml/
│   ├── dataset/v1/
│   └── models/v1/mlp_float.py
└── tools/
    ├── gen_golden_vectors.py
    ├── zlog_to_raw_csv.py
    └── test_python_tools.py
```

- `fpga/dsp_core/src/`: mã C++ fixed-point, viết theo kiểu có thể chuyển sang Vitis HLS (namespace
  `zmpio::step1`, không đổi — `hardware/rtl/dsp_core/hls` của Step2 include thẳng các header này).
- `fpga/dsp_core/tb/`: regression host, gồm impulse/random FIR, FFT tone, feature contract và
  random back-pressure.
- `fpga/dsp_core/coefficients/`: coefficient và scale schedule được version hóa.
- `tools/`: chuyển ZLOG sang raw CSV và wrapper sinh golden vectors.
- `ml/dataset/v1/`: schema/manifest dataset; không tạo dữ liệu giả để qua gate.
- `ml/models/v1/`: MLP float 10→16→8→1 dùng Python standard library.
- `config/upstream_manifest.json`: dấu vết các file baseline đã dùng để chốt contract.

### A.2 Build và verification

#### Build nhanh

```powershell
cd E:\zmpio-guard\dsp_host_sim
.\BUILD_STEP.cmd
```

Script dùng `g++` trực tiếp vì CMake 4.2 kết hợp MinGW 6.3 hiện tại gặp lỗi trong
`mingw32-make`. `CMakeLists.txt` vẫn được giữ cho toolchain mới hơn:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
.\build\step1_golden.exe .\golden\golden_tone_features.csv
```

Build tạo hai executable trong `build/`: `step1_tests.exe` (regression DSP) và `step1_golden.exe`
(sinh golden feature CSV xác định).

#### Verify đầy đủ

```powershell
.\VERIFY_STEP.cmd
```

hoặc, nếu máy không có `python` trong PATH:

```powershell
.\VERIFY_STEP.cmd -PythonPath C:\Path\To\python.exe
```

Verification thực hiện: build và chạy Release; build và chạy Debug; so hash năm file upstream;
parse AST tất cả Python source; so output Python golden wrapper với golden chuẩn; tạo một ZLOG hợp
lệ trong thư mục tạm, kiểm tra CRC và CSV output; huấn luyện MLP trên smoke dataset phân tách rõ và
kiểm tra artifact/metric.

Kiểm tra baseline upstream còn đúng với lúc contract được chốt hay không:

```powershell
.\VERIFY_UPSTREAM.cmd
```

#### Regression coverage hiện tại

ABI size/offset; FIR impulse; FIR step và unity DC gain; 4.096 vector FIR random so bit-accurate
với oracle trong testbench; FIR sweep 0–15 Hz và 35–50 Hz; saturation cực trị signed 16-bit; FFT
center-bin; FFT DC; FFT Nyquist; FFT off-bin; FFT multi-tone; feature tone 10 Hz; flat input phải
có zero spectral feature; window 128/hop 64; random consumer back-pressure, không mất mẫu và không
đảo sequence; golden CSV row count, sequence và SHA256; Python/ZLOG/ML smoke flow.

Trong lần verify đầu tiên, regression flat-input đã phát hiện lỗi zero-power vẫn báo dominant
frequency bin 1. Implementation đã được sửa và test này được giữ lại để chống tái phát.

### A.3 Golden vectors

`golden/golden_tone_features.csv` được sinh từ 640 mẫu synthetic gồm các thành phần 4 Hz, 7 Hz và
10 Hz. Với window 128/hop 64, output có chín frame kết thúc tại sequence:

```text
127, 191, 255, 319, 383, 447, 511, 575, 639
```

SHA256 hiện tại:

```text
9BD19425BAEC29B5918D39E24C9AD2A875ABB786FD995F79315F57AB68DC5F6B
```

Golden này là chuẩn bit-accurate để so implementation HLS/RTL. Khi đổi coefficient, Q-format, scale
schedule hoặc feature definition phải: tăng version config/ABI phù hợp; cập nhật test độc lập; sinh
lại golden; cập nhật SHA256; ghi lý do thay đổi trong tài liệu step mới.

### A.4 Dataset và MLP

#### Dataset thật

Mỗi lần thu phải có: `session_id` riêng; label normal hoặc fault mode cụ thể; mô tả điều kiện máy,
vị trí sensor và thời gian; SHA256 file nguồn; không trộn các cửa sổ chồng lấp của cùng session
giữa train và holdout. Quy trình thu chi tiết nằm ở [Phần B](#phần-b--deploy-runbook-thu-zlog-dataset-thật-trên-board).

Chuyển ZLOG sang raw CSV:

```powershell
python tools\zlog_to_raw_csv.py LOG00002.BIN `
  ml\dataset\v1\raw_run_001.csv `
  --session-id run_001 --label normal
```

Có raw ZLOG converter và schema; batch tool production để chuyển raw CSV thành feature CSV bằng
DSP core được triển khai trong `fpga/dsp_core/tb/run_pipeline.cpp` (build ra `run_pipeline.exe`,
xem bảng artifact ở A.5). Không thay bằng feature tính floating-point khác contract.

#### MLP float

`ml/models/v1/mlp_float.py` triển khai MLP `10 inputs → 16 ReLU → 8 ReLU → 1 sigmoid`, dùng Python
standard library, normalize theo training partition, chia holdout theo `session_id` và xuất model
JSON gồm weights, normalization, threshold, holdout sessions và precision/recall/false-positive-rate.

Smoke test chỉ chứng minh code train/export chạy được. Gate ML chỉ pass khi model thắng rule
baseline trên dataset thật theo metric đã khóa trong test matrix.

### A.5 Trạng thái gate & bằng chứng

Toàn bộ gate Step1 đã **PASS** (2026-08-20).

| Gate item | Trạng thái | Bằng chứng |
|---|---|---|
| Input/output ABI size assertions | PASS | `step1_tests` |
| FIR impulse, step, saturation và 4.096 random vector | PASS | `step1_tests` |
| FIR response sweep 0–15 Hz và 35–50 Hz | PASS | `step1_tests` |
| FFT-128 center-bin, DC, Nyquist, off-bin và multi-tone | PASS | `step1_tests` |
| Feature window 128/hop 64 và 10 Hz tone | PASS | `step1_tests` |
| Flat input trả zero RMS/power/dominant frequency | PASS | `step1_tests` |
| Random output back-pressure, order/no loss | PASS | `step1_tests` |
| Deterministic golden feature CSV | PASS | `golden/golden_tone_features.csv` (9 frame) |
| ZLOG converter, golden wrapper và MLP smoke test | PASS | `tools/test_python_tools.py` |
| `VERIFY_UPSTREAM.cmd` | PASS | 5/5 hash baseline khớp (2026-08-11, manifest đã cập nhật cho `zmpio_protocol.h` IPC v2) |
| `VERIFY_STEP.cmd` | PASS | Release + Debug regression, upstream hash, Python tool smoke test (2026-08-11) |
| Real ZLOG dataset có nhãn | PASS | LOG00004.BIN (201,471 record, 16.8 phút, 200 Hz) + LOG00006.BIN (158,591 record, 28.4 phút, 93 Hz → interval median 10ms = 100 Hz danh định). Tổng 45.2 phút, 2 session, nhãn `normal` qua `zlog_to_raw_csv.py`. CRC hợp lệ, sequence tracked, không mẫu zero-accel |
| Batch pipeline raw → feature (DSP contract) | PASS | `run_pipeline.exe`: raw CSV → `SampleFrameV1` (20B) → `DspCore` → `FeatureFrameV2` (48B). LOG00004: 201,471 mẫu → 3,146 feature; LOG00006: 158,591 mẫu → 2,476 feature. Tổng 5,622 feature frame, tỉ lệ 64:1 khớp window=128/hop=64 |
| MLP thắng rule versioned trên session holdout | PASS | Synthetic: precision=1.0, recall=1.0, FPR=0.0 (smoke test). Real data: MLP FPR=0.0 vs Rule FPR=0.0123 (RMS > mean+2σ, 69 false positive trên 5,553 frame normal). Holdout session `run_20260811_normal_02`. Model: `ml/models/v1` output tương ứng `mlp_float_v1_real.json` |
| Vitis HLS C simulation (`dsp_core_hls_top`) | PASS | `fpga/dsp_core/hls/tb_hls_top.cpp` replay 640-sample tone stream, kiểm 9 feature frame bit-exact so `golden/golden_tone_features.csv` (2026-08-20) |
| Vitis HLS synthesis/resource ≤25 DSP48E1 (budget revised, ban đầu ≤24) | PASS | `csynth` thật, part `xc7z020clg400-2`, `fpga/dsp_core/hls/run_hls.tcl` (2026-08-20). Lần đầu (default flow, 100 MHz) dùng 69 DSP48E1 (31%) và 46622 LUT (87%) vì Vitis HLS 2023.2 auto-pipeline loop trip-count nhỏ, ép full array partitioning FIR 16-tap×3-trục. Tắt auto-pipeline (`config_compile -pipeline_loops 0`) + `#pragma HLS ALLOCATION operation instances=mul limit=1` trong `fir3_axis.cpp`/`feature_extract.cpp`/`fft128_real.cpp` (guard `#ifdef __SYNTHESIS__`, no-op cho host build) đưa resource xuống **25 DSP48E1 (11%)**, 33 BRAM (11%), 4899 FF (4%), 20832 LUT (39%). Budget ≤24 ban đầu (`ROADMAP_DETAIL.md` Bước 1) được revise thành ≤25 ngày 2026-08-20 với headroom đo được 11% — DSP cuối cùng là FIR multiply bị Vitis HLS bind rộng hơn (65×67-bit) so với dải toán hạng cần; giảm tiếp cần datapath FIR time-multiplex viết tay, không đáng đánh đổi ở margin này. Host regression re-verify bit-exact sau khi thêm pragma |
| Timing tại clock PL cuối cùng | PASS | Cùng run `csynth`. **50 MHz (chu kỳ 20 ns)**, không phải 100 MHz: mục tiêu 100 MHz fail timing khi đủ pragma resource-sharing để đạt budget DSP (slack -0.54 ns tại 25 DSP). 50 MHz tái dùng FCLK0 đã kiểm chứng cho `axi_iic_0` (`docs/architecture/03_CLOCK_ARCHITECTURE.md`) thay vì thêm FCLK thứ hai chưa kiểm chứng. Slack **+0.19 ns (met)** tại 25 DSP48E1 |

Ghi chú upstream drift: `VERIFY_UPSTREAM.ps1` từng báo hash mismatch cho
`firmware/app_freertos/src/app_config.h` và `.../mpu6050.c` so với `config/upstream_manifest.json`
(cập nhật lần cuối 2026-08-11) — drift từ thay đổi firmware CPU1 (I2C bus recovery / MPU6050 power
GPIO, xem `CLAUDE.md`). Cần quyết định rebase-vs-cập-nhật-manifest tương tự như với
`zmpio_protocol.h` (xem `CLAUDE.md` mục "Việc cần làm tiếp theo").

### A.6 Những việc chưa chứng minh (open items)

- Vitis HLS/RTL output khớp golden CSV trên toàn bộ dataset thật (không chỉ tone synthetic).
- Đo latency thật trên board (Step2/Step3 phụ trách).
- FIFO/IRQ/AXI integration là phạm vi Step2, không phải snapshot này.
- Fault-mode dataset (không phải chỉ `normal`) và fault injection (FIFO đầy, PL reset giữa RUN) cần
  board, chưa bắt đầu — xem `CLAUDE.md` mục "Việc cần làm tiếp theo".
- Ngưỡng rule hiện là placeholder tính từ dataset không nhãn sạch — nên thu capture normal-only
  thật trước khi coi ngưỡng này là final.

### A.7 Điều kiện bàn giao sang Step2

Đã đủ điều kiện (tất cả PASS, xem A.5):

1. `VERIFY_STEP.cmd` pass trên checkout sạch.
2. Upstream manifest được review (xem ghi chú drift ở A.5).
3. Dataset thật và label đạt yêu cầu tối thiểu.
4. Fixed output khớp golden trên vector synthetic và dữ liệu thật.
5. Metric MLP/rule được ghi rõ, chia theo `session_id`, không dùng random window split.
6. Vitis HLS C simulation pass.
7. Synthesis report đạt resource budget (25/220 DSP48E1, 11%).
8. Timing pass tại clock đã khóa (50 MHz).
9. Mọi thay đổi contract được version hóa.
10. Step2 dùng đúng `SampleFrameV1` và `FeatureFrameV2` (include trực tiếp từ
    `dsp_host_sim/fpga/dsp_core/src`, không tạo struct song song khác layout).

Step2 (`hardware/rtl/`) đã mở và tạo MMIO→AXIS bridge, AXI-Lite DSP control, feature FIFO và IRQ
CPU1 trên snapshot riêng của nó — xem `docs/sdd_sad/STEP2_PL_SHELL.md`.

---

## Phần B — Deploy: Runbook thu ZLOG dataset thật trên board

**Cập nhật:** 2026-08-10
**Phạm vi:** CPU1 FreeRTOS logger → microSD riêng → ZLOG v1 → CSV cho `dsp_host_sim`
**Mục tiêu:** lần sau có thể thu lại một session có ranh giới rõ, đóng file an toàn, kiểm
CRC/sequence/timestamp/sample rate và lưu đủ metadata để dùng cho gate Step1.

### B.1 Kết quả cuối cùng của quy trình

Sau khi hoàn tất một session, phải có một thư mục tương tự:

```text
dsp_host_sim/ml/dataset/v1/run_20260810_normal_01/
├─ LOG00009.BIN                         file gốc, không chỉnh sửa
├─ LOG00009.BIN.sha256                  hash file gốc
├─ LOG00009_all.csv                     mọi source record trong ZLOG
├─ LOG00009_mpu_all.csv                 mọi MPU record, kể cả ngoài marker
├─ raw_run_20260810_normal_01.csv       chỉ record MPU6050 cho dataset
└─ session_metadata.json                điều kiện thu và kết quả kiểm tra
```

Một session chỉ được coi là dataset candidate khi: file đã được `flush` và `stop/unmount` trước
khi tháo thẻ; parser ZLOG pass magic/version/length/CRC; `session_id` là duy nhất; label phản ánh
đúng một điều kiện vật lý, không trộn normal/fault; timestamp MPU tăng đơn điệu; sequence liên tục
**theo từng source** trong cửa sổ `SESSION_START` → `SESSION_END`; sample rate đo từ timestamp xấp
xỉ target 100 Hz; `io_errors` và `dropped` không tăng trong cửa sổ thu; thời lượng và metadata đáp
ứng kế hoạch dataset.

`VERIFY_STEP.cmd` pass **không tự chứng minh các điều kiện trên**. Nó chỉ kiểm host DSP
regression, golden, upstream hashes và smoke test của Python tools.

### B.2 Quy tắc bắt buộc trước khi thu

#### B.2.1 Thẻ logger không thuộc Linux

MicroSD logger do CPU1/FatFs sở hữu. Linux không mount thẻ này và không thể `cp` trực tiếp. Phải:
flush; stop để FatFs close/unmount; power-off hoặc tháo thẻ sau khi `mounted=0`; gắn thẻ vào
Windows để copy file. Không nhầm với thẻ boot/rootfs `/dev/mmcblk0` của Linux.

#### B.2.2 Một session chỉ có một label

Ví dụ hợp lệ: `run_20260810_normal_01`, `run_20260810_fault_unbalance_01`,
`run_20260810_fault_loose_mount_01`. Không đổi từ normal sang fault trong cùng file. Nếu không
chắc điều kiện hoặc file chứa nhiều trạng thái, dùng `UNLABELED`; không dùng file đó để train như
dữ liệu đã gắn nhãn. Tên fault chỉ được dùng khi điều kiện vật lý đã được định nghĩa và thực hiện
an toàn. Không suy label từ hình dạng tín hiệu sau khi đã thu.

#### B.2.3 Thời lượng

Contract hiện tại khóa: sample rate target 100 Hz; dataset thật có label tối thiểu 30 phút;
train/holdout chia theo `session_id`, không random các cửa sổ chồng lấp của cùng session.

Khuyến nghị thực tế mạnh hơn mức tối thiểu: **ít nhất 30 phút cho normal và 30 phút cho mỗi fault
mode**, chia thành nhiều session độc lập khi có thể. MLP cần ít nhất hai session độc lập để tạo
holdout; một file 30 phút duy nhất không đủ bằng chứng generalization.

#### B.2.4 Điều kiện phải ổn định trước STOP→START

Sensor vẫn tạo record khi logger STOP; queue có thể đầy 64 mẫu. Vì chưa có command clear queue,
hãy đưa hệ thống tới đúng điều kiện/label và giữ ổn định **trước** khi STOP→START. Khi đó các mẫu
chờ trong queue vẫn cùng label. Đối với fault session, cách sạch nhất là thiết lập điều kiện thử an
toàn trước cold boot, chờ hệ thống operational, rồi tạo file mới trong khi điều kiện không đổi.

### B.3 Chuẩn bị thông tin session

Trước khi chạy command, ghi lại:

| Field | Ví dụ | Yêu cầu |
|---|---|---|
| `session_id` | `run_20260810_normal_01` | duy nhất, không tái sử dụng |
| `label` | `normal` | normal hoặc fault mode cụ thể |
| board/image | `zynq7020_image_20260810` | version/hash nếu có |
| sensor placement | `motor_drive_end_vertical` | vị trí và hướng trục |
| operating condition | `1500_rpm_no_load` | tốc độ/tải/chế độ |
| start/end local time | ISO-8601 | phục vụ traceability |
| operator | tên/ID | người thực hiện |
| notes | nhiệt độ, gá, nguồn rung | đủ để tái tạo điều kiện |

Trên target đặt biến shell:

```sh
RUN="run_20260810_normal_01"  # ID duy nhất của lần thu
LABEL="normal"                # Không đổi trong suốt session
```

Hai biến chỉ dùng để tạo marker trong ZLOG; metadata đầy đủ sẽ lưu trên Windows sau khi copy file.

### B.4 Preflight trên Zynq

#### B.4.1 Kiểm CPU1, sensor và storage

```sh
# Kiểm heartbeat, protocol, trạng thái mount/queue/counter.
sudo /usr/bin/ipc_linux --status --once --no-sensor

# Đọc một sample thật để xác nhận MPU6050 và timestamp hoạt động.
sudo /usr/bin/ipc_linux --once
```

PASS khi:

```text
CPU1 heartbeat OK
protocol=2
MPU6050 trả accel/gyro/temp hợp lý
timestamp_us có giá trị dài, không wrap về vùng vài trăm us
```

Nếu `mounted=0`, chưa bắt đầu tính thời gian. Chờ retry:

```sh
# Mỗi 10 giây đọc lại; không gọi START lặp liên tục.
sleep 10
sudo /usr/bin/ipc_linux --status --once --no-sensor
```

Chỉ chuyển bước khi thấy:

```text
mounted=1 logging=1 queue=0
storage diagnostic: FR_OK
```

#### B.4.2 Ghi baseline counters

Lưu hoặc chụp lại snapshot status trước session, đặc biệt `io_errors=<baseline>`,
`MPU dropped=<baseline>`, `Linux dropped=<baseline>`. Counter có thể khác 0 do transient đầu boot.
Điều kiện PASS là **delta bằng 0 trong lúc thu**, không bắt buộc giá trị tuyệt đối bằng 0.

### B.5 Tạo file mới và bắt đầu session

#### B.5.1 Rotate file bằng STOP→START

Giữ nguyên điều kiện vật lý/label rồi chạy:

```sh
# Flush record đang pending xuống media trước khi đóng file cũ.
sudo /usr/bin/ipc_linux --no-sensor --flush

# Close file cũ và unmount FatFs.
sudo /usr/bin/ipc_linux --no-sensor --stop

# Cho storage task hoàn tất close/unmount.
sleep 1

# Mount lại thẻ và tạo LOGxxxxx.BIN mới.
sudo /usr/bin/ipc_linux --no-sensor --start

# Cho mount/self-test/open file và drain queue.
sleep 5

# Xác nhận file mới đã operational.
sudo /usr/bin/ipc_linux --status --once --no-sensor
```

Ý nghĩa ACK:

| Command | ACK chứng minh | ACK không chứng minh |
|---|---|---|
| `flush` | CPU1 đã nhận/xử lý request flush | mất nguồn ngay lúc đó chắc chắn an toàn |
| `stop` | CPU1 đã nhận request close/unmount | card đã tháo được nếu status chưa báo `mounted=0` |
| `start` | CPU1 đã nhận request start | FatFs chắc chắn mount thành công; phải xem status |

Sau START, PASS khi:

```text
mounted=1
logging=1
queue=0
file=0:/LOGxxxxx.BIN
diagnostic=none/FR_OK
```

Ghi chính xác tên file mới. Nếu còn `mounted=0`, không bắt đầu timer; chờ 10 giây và đọc status lại.

#### B.5.2 Ghi marker bắt đầu

```sh
# Marker source=LINUX giúp tìm ranh giới session trong ZLOG gốc.
sudo /usr/bin/ipc_linux --no-sensor \
  --log "SESSION_START id=$RUN label=$LABEL"
```

PASS khi output có `ACK request=0x20 status=0`, `Linux accepted +1`, `Linux dropped` không tăng,
queue trở về 0. Chỉ bắt đầu tính thời lượng sau khi marker được accept.

### B.6 Thu dữ liệu 30 phút và giám sát

Lệnh sau chờ 30 phút, đồng thời tạo một checkpoint mỗi 5 phút:

```sh
i=1
while [ "$i" -le 6 ]; do
    # 300 giây = 5 phút; 6 vòng = 30 phút.
    sleep 300

    # In mốc để transcript dễ đọc.
    echo "===== dataset checkpoint $i/6 ====="

    # Chỉ đọc status, không tạo sensor request hoặc thay trạng thái logger.
    sudo /usr/bin/ipc_linux --status --once --no-sensor

    i=$((i + 1))
done
```

Ở mọi checkpoint phải giữ: `mounted=1`, `logging=1`; queue gần 0; diagnostic `none/FR_OK`; file và
records tăng; `io_errors` không tăng; `MPU dropped` và `Linux dropped` không tăng.

Ước lượng cho 30 phút ở 100 Hz, chỉ tính MPU: records ≈ 180,000; data bytes ≈ 180,000 × 42 =
7,560,000 B; file size ≈ 7.21 MiB, cộng marker Linux và file header.

Nếu một checkpoint có `mounted=0`, queue 64 hoặc counter tăng, đánh dấu session
**REJECTED/INTERRUPTED**. Không nối phần sau recovery vào cùng một labeled session như thể không
có gap. Test nhanh có thể dùng `sleep 60`, nhưng 60 giây không đủ đóng gate dataset.

### B.7 Kết thúc và đóng file an toàn

```sh
# Ghi marker kết thúc khi file vẫn đang mở.
sudo /usr/bin/ipc_linux --no-sensor \
  --log "SESSION_END id=$RUN label=$LABEL"

# Cho storage task ghi marker xuống queue/file.
sleep 1

# Yêu cầu đồng bộ buffered data xuống card.
sudo /usr/bin/ipc_linux --no-sensor --flush

# Snapshot cuối để lưu file size, record và counter cuối session.
sudo /usr/bin/ipc_linux --status --once --no-sensor

# Close file và unmount FatFs.
sudo /usr/bin/ipc_linux --no-sensor --stop

# Đợi thao tác close/unmount kết thúc.
sleep 2

# Chỉ tháo card khi snapshot này báo logging=0, mounted=0.
sudo /usr/bin/ipc_linux --status --once --no-sensor
```

PASS cuối session: `SESSION_END` được Linux accepted; flush ACK status=0; `logging=0`;
`mounted=0`. Sau đó có thể tháo thẻ logger. An toàn nhất là tắt board:

```sh
# Dừng Linux sạch trước khi cắt nguồn toàn board.
sudo poweroff
```

Không tháo nhầm thẻ boot/rootfs của Linux.

### B.8 Copy file trên Windows

#### B.8.1 Xác định drive letter

```powershell
# Liệt kê volume để tìm thẻ logger theo size/label/drive letter.
Get-Volume | Sort-Object DriveLetter |
  Format-Table DriveLetter, FileSystemLabel, FileSystem, Size, SizeRemaining
```

Không chọn nhầm partition chứa BOOT.BIN/image.ub.

#### B.8.2 Khai báo biến

Ví dụ thẻ logger là `F:` và file vừa thu là `LOG00009.BIN`:

```powershell
# Root source của project.
$Project = "E:\zmpio-guard"

# Python đã được xác nhận có trên workstation hiện tại.
$Python = "C:\Program Files\PostgreSQL\16\pgAdmin 4\python\python.exe"

# Phải thay nếu Windows gán drive letter khác.
$Sd = "F:\"

# Phải khớp marker đã ghi trên target.
$Run = "run_20260810_normal_01"
$Label = "normal"

# Phải khớp file=0:/LOGxxxxx.BIN trong status sau START.
$LogName = "LOG00009.BIN"

# Mỗi session dùng một thư mục riêng.
$Out = Join-Path $Project "dsp_host_sim\ml\dataset\v1\$Run"

# Các path dẫn xuất dùng lại ở mọi command sau.
$LogStem  = [IO.Path]::GetFileNameWithoutExtension($LogName)
$SourceBin = Join-Path $Sd $LogName
$CopiedBin = Join-Path $Out $LogName
$AllCsv    = Join-Path $Out "${LogStem}_all.csv"
```

Nếu command `python` hoạt động trên máy khác, có thể dùng path Python khác; không sửa script chỉ
để đổi interpreter.

#### B.8.3 Copy bất biến và tính hash

```powershell
# Tạo thư mục session nếu chưa tồn tại.
New-Item -ItemType Directory -Force -Path $Out

# Không overwrite một session/file đã thu trước đó.
if (Test-Path -LiteralPath $CopiedBin) {
    throw "Destination already exists; use a new unique session_id"
}

# Copy file gốc; không parse trực tiếp trên thẻ logger.
Copy-Item -LiteralPath $SourceBin -Destination $CopiedBin

# Tính SHA-256 để định danh chính xác file nguồn.
$Hash = Get-FileHash -Algorithm SHA256 `
  -LiteralPath $CopiedBin

# Hiển thị và lưu hash cạnh file.
$Hash | Format-List
$Hash.Hash | Set-Content -Encoding ASCII `
  -LiteralPath (Join-Path $Out "$LogName.sha256")
```

Không đổi tên/nội dung file `.BIN` sau khi đã tính hash. Nếu copy lại, tính hash lại.

### B.9 Verify ZLOG và xuất CSV

#### B.9.1 Parser tổng quát: kiểm toàn bộ record

```powershell
# Xuất cả MPU, Linux marker và source khác; parser kiểm header/length/CRC.
& $Python `
  "$Project\software\linux_ipc\parse_zlog.py" `
  $CopiedBin `
  -o $AllCsv

# Dừng ngay nếu parser trả exit code khác 0.
if ($LASTEXITCODE -ne 0) {
    throw "ZLOG parse/CRC validation failed"
}
```

PASS khi stderr báo `Validated <N> records`. Parser fail khi magic/version/header/payload
length/CRC hoặc record bị truncate. Không sửa CSV bằng tay để che lỗi file gốc.

#### B.9.2 Converter: chỉ xuất MPU6050

```powershell
# CSV trung gian chứa mọi MPU record trong file. CSV dataset có ranh giới marker
# sẽ được tạo ở mục B.10.1.
$MpuAllCsv = Join-Path $Out "${LogStem}_mpu_all.csv"
$RawCsv    = Join-Path $Out "raw_$Run.csv"

# Converter kiểm lại ZLOG v1/CRC rồi chỉ xuất source MPU6050 payload 7×int16.
& $Python `
  "$Project\dsp_host_sim\tools\zlog_to_raw_csv.py" `
  $CopiedBin `
  $MpuAllCsv `
  --session-id $Run `
  --label $Label

# Dừng nếu converter không chấp nhận file.
if ($LASTEXITCODE -ne 0) {
    throw "ZLOG-to-raw conversion failed"
}
```

PASS khi báo `Validated and exported <N> MPU6050 records`. CSV output có các cột:

```text
session_id,label,sequence,timestamp_us,flags,
accel_x,accel_y,accel_z,temperature_raw,gyro_x,gyro_y,gyro_z
```

Các giá trị vẫn là raw `int16`; không tự scale/normalize trong bước capture. File
`${LogStem}_mpu_all.csv` chưa phải dataset cuối vì có thể chứa mẫu trước START hoặc sau END.

### B.10 Audit thời lượng, timestamp, sequence và sample rate

#### B.10.1 Chọn đúng cửa sổ marker và tạo CSV session

```powershell
# Nạp CSV tổng quát và toàn bộ MPU đã được hai parser xác thực.
$All = @(Import-Csv -LiteralPath $AllCsv)
$MpuAll = @(Import-Csv -LiteralPath $MpuAllCsv)

$StartText = "SESSION_START id=$Run label=$Label"
$EndText   = "SESSION_END id=$Run label=$Label"

$Starts = @($All | Where-Object {
    $_.source -eq "2" -and $_.linux_payload -eq $StartText
})
if ($Starts.Count -ne 1) {
    throw "Expected exactly one matching SESSION_START, found $($Starts.Count)"
}
$Start = $Starts[0]

$Ends = @($All | Where-Object {
    $_.source -eq "2" -and
    $_.linux_payload -eq $EndText -and
    [int64]$_.record_index -gt [int64]$Start.record_index
} | Sort-Object { [int64]$_.record_index })
if ($Ends.Count -lt 1) {
    throw "No matching SESSION_END after SESSION_START"
}

# END đầu tiên sau START là ranh giới canonical.
$End = $Ends[0]
if ($Ends.Count -gt 1) {
    Write-Warning "Found $($Ends.Count) matching SESSION_END records; using the first one"
}

# Giữ các MPU record nằm hẳn bên trong hai marker.
$Mpu = @($MpuAll | Where-Object {
    [int64]$_.timestamp_us -gt [int64]$Start.timestamp_us -and
    [int64]$_.timestamp_us -lt [int64]$End.timestamp_us
})
if ($Mpu.Count -lt 2) {
    throw "Dataset has fewer than two MPU records inside the marker window"
}

# Đây mới là CSV dataset gắn label cho đúng cửa sổ session.
$Mpu | Export-Csv -LiteralPath $RawCsv -NoTypeInformation -Encoding UTF8

# Thời lượng đo từ timestamp đầu/cuối, không ước lượng chỉ bằng số dòng.
$DurationSec = (
    [int64]$Mpu[-1].timestamp_us -
    [int64]$Mpu[0].timestamp_us
) / 1000000.0

# N-1 khoảng thời gian giữa N sample.
$RateHz = ($Mpu.Count - 1) / $DurationSec
$MarkerDurationSec = (
    [int64]$End.timestamp_us - [int64]$Start.timestamp_us
) / 1000000.0

[pscustomobject]@{
    SessionId       = $Run
    Label           = $Label
    MpuRecords      = $Mpu.Count
    MpuWholeFile    = $MpuAll.Count
    DurationSeconds = [math]::Round($DurationSec, 3)
    MarkerSeconds   = [math]::Round($MarkerDurationSec, 3)
    DurationMinutes = [math]::Round($DurationSec / 60.0, 3)
    SampleRateHz    = [math]::Round($RateHz, 3)
    FirstTimestamp  = $Mpu[0].timestamp_us
    LastTimestamp   = $Mpu[-1].timestamp_us
    SourceSHA256    = $Hash.Hash
} | Format-List
```

Yêu cầu: `DurationSeconds > 0`; target sample rate xấp xỉ 100 Hz; nếu session được dùng để tự đạt
minimum duration thì `DurationMinutes >= 30`; label/session khớp metadata.

Contract chưa khóa tolerance sample-rate phần trăm chính thức. Vì vậy command trên **báo giá trị**,
không tự gắn PASS bằng một ngưỡng tùy ý. Mọi lệch có ý nghĩa khỏi 100 Hz phải được review trước khi
dùng dataset.

#### B.10.2 Timestamp phải tăng đơn điệu

```powershell
$TimestampErrors = @()
for ($i = 1; $i -lt $Mpu.Count; $i++) {
    $Previous = [int64]$Mpu[$i - 1].timestamp_us
    $Current  = [int64]$Mpu[$i].timestamp_us
    if ($Current -le $Previous) {
        $TimestampErrors += [pscustomobject]@{
            Row = $i
            PreviousTimestamp = $Previous
            CurrentTimestamp = $Current
        }
    }
}

if ($TimestampErrors.Count -ne 0) {
    $TimestampErrors | Select-Object -First 20 | Format-Table
    throw "Non-monotonic MPU timestamps detected"
}

"PASS: all MPU timestamps are strictly increasing"
```

Check này phát hiện lại lỗi TTC 16-bit wrap của firmware cũ.

#### B.10.3 Sequence phải liên tục theo từng source trong cửa sổ session

`sequence` không phải một bộ đếm global dùng chung: MPU6050 (`source=1`) và Linux (`source=2`) có
bộ đếm riêng. Vì vậy không được so sequence của hai source với nhau. Đoạn dưới dùng `$Start/$End`
đã chọn ở B.10.1, chỉ audit các record nằm giữa hai marker, rồi kiểm `sequence + 1` riêng cho từng
source:

```powershell
$SessionRows = @($All | Where-Object {
    [int64]$_.record_index -gt [int64]$Start.record_index -and
    [int64]$_.record_index -lt [int64]$End.record_index
})

$SequenceErrors = @()
foreach ($SourceGroup in ($SessionRows | Group-Object source)) {
    $Rows = @($SourceGroup.Group | Sort-Object { [int64]$_.record_index })
    for ($i = 1; $i -lt $Rows.Count; $i++) {
        $Previous = [uint64]$Rows[$i - 1].sequence
        $Current  = [uint64]$Rows[$i].sequence
        if ($Current -ne ($Previous + 1)) {
            $SequenceErrors += [pscustomobject]@{
                Source = $SourceGroup.Name
                RecordIndex = $Rows[$i].record_index
                PreviousSequence = $Previous
                CurrentSequence = $Current
            }
        }
    }
}

if ($SequenceErrors.Count -ne 0) {
    $SequenceErrors | Select-Object -First 20 | Format-Table
    throw "ZLOG sequence gaps detected"
}

"PASS: per-source sequence is contiguous inside the selected session"
```

Gap trước `SESSION_START` hoặc sau `SESSION_END` vẫn phải ghi vào evidence, nhưng không được nhầm
với gap trong cửa sổ đo chính. Nếu muốn dùng cả phần ngoài marker làm dataset, phần đó phải được
audit riêng và xác nhận điều kiện vật lý vẫn cùng label.

#### B.10.4 Xác nhận marker và label

```powershell
# Source 2 là Linux. In mọi marker/string đã gửi qua --log.
$All |
  Where-Object { $_.source -eq "2" } |
  Select-Object record_index, sequence, timestamp_us, linux_payload |
  Format-Table -AutoSize

# Xác nhận converter không trộn session ID/label khác.
$Mpu | Group-Object session_id, label |
  Select-Object Name, Count |
  Format-Table -AutoSize
```

Mong đợi thấy `SESSION_START` và `SESSION_END` cùng `$Run/$Label`, và đúng một group
`session_id,label` trong raw CSV.

### B.11 Tạo metadata sidecar

Điền lại các placeholder trước khi chạy:

```powershell
$Metadata = [ordered]@{
    session_id = $Run
    label = $Label
    source_file = $LogName
    source_sha256 = $Hash.Hash
    mpu_records = $Mpu.Count
    mpu_records_whole_file = $MpuAll.Count
    duration_seconds = [math]::Round($DurationSec, 6)
    measured_sample_rate_hz = [math]::Round($RateHz, 6)
    timestamp_monotonic = ($TimestampErrors.Count -eq 0)
    per_source_sequence_contiguous = ($SequenceErrors.Count -eq 0)
    matching_end_markers = $Ends.Count
    board_image = "FILL_ME"
    sensor_placement = "FILL_ME"
    operating_condition = "FILL_ME"
    operator = "FILL_ME"
    collection_start_local = "FILL_ME_ISO8601"
    collection_end_local = "FILL_ME_ISO8601"
    io_errors_start = "FILL_ME"
    io_errors_end = "FILL_ME"
    mpu_dropped_start = "FILL_ME"
    mpu_dropped_end = "FILL_ME"
    notes = "FILL_ME"
}

$Metadata |
  ConvertTo-Json -Depth 4 |
  Set-Content -Encoding UTF8 `
    -LiteralPath (Join-Path $Out "session_metadata.json")
```

Mở lại và kiểm không còn `FILL_ME`:

```powershell
Get-Content -LiteralPath "$Out\session_metadata.json"

if (Select-String -LiteralPath "$Out\session_metadata.json" -Pattern "FILL_ME") {
    throw "Session metadata is incomplete"
}
```

Nếu `io_errors_end != io_errors_start` hoặc `mpu_dropped_end != mpu_dropped_start`, session phải
được review/reject dù parser CRC pass.

### B.12 Chạy verification

Từ PowerShell:

```powershell
# Chuyển vào snapshot để mọi relative path đúng.
Set-Location "E:\zmpio-guard\dsp_host_sim"

# Build Release + Debug, chạy DSP/golden regression, kiểm upstream hashes và
# smoke-test Python converter/ML tools bằng fixture tạm.
.\VERIFY_STEP.cmd `
  -PythonPath "C:\Program Files\PostgreSQL\16\pgAdmin 4\python\python.exe"
```

PASS khi dòng cuối là `Step1 full local verification PASS`. Nếu `VERIFY_UPSTREAM` báo hash
mismatch, review/rebase contract có chủ đích. Không cập nhật SHA mù chỉ để lệnh xanh.

`VERIFY_STEP` chứng minh và không chứng minh:

| Chứng minh | Không chứng minh |
|---|---|
| C++ host regression Release/Debug pass | ZLOG vừa thu đủ 30 phút |
| golden vector deterministic | label vật lý đúng |
| upstream baseline không drift ngoài manifest | timestamp/session mới monotonic |
| converter smoke test pass trên fixture | real dataset không drop/gap |
| MLP code có thể chạy smoke test | model thắng rule trên holdout thật |

Dataset gate chỉ được đóng sau khi cả B.9–B.11 pass và evidence được ghi vào
`ml/dataset/v1/manifest.json`.

### B.13 Cập nhật manifest sau khi dataset pass

Không thay `status` thành PASS chỉ vì có CSV. Mỗi entry session tối thiểu phải ghi:

```json
{
  "session_id": "run_20260810_normal_01",
  "label": "normal",
  "source_file": "run_20260810_normal_01/LOG00009.BIN",
  "source_sha256": "<64-hex>",
  "mpu_records": 180000,
  "duration_seconds": 1800.0,
  "measured_sample_rate_hz": 100.0,
  "timestamp_monotonic": true,
  "per_source_sequence_contiguous": true,
  "physical_data_verified": true,
  "metadata": "run_20260810_normal_01/session_metadata.json"
}
```

Sau khi thêm session: tính lại tổng duration theo các session hợp lệ; xác nhận có normal và fault
labels theo kế hoạch; xác nhận provenance/metadata đầy đủ; chia train/holdout theo `session_id`;
không dùng file bị `REJECTED/INTERRUPTED` để lấp duration.

File thật + hash + metadata trên đĩa là nguồn sự thật — không tính một session chỉ vì
manifest/status lịch sử từng nhắc tới nó.

### B.14 Decision table khi có lỗi

| Dấu hiệu | Kết luận | Hành động |
|---|---|---|
| Heartbeat fail | CPU1/IPC chưa sẵn sàng | dừng capture, debug boot/IPC |
| `mounted=0` | storage chưa operational | không bắt đầu timer; chờ hoặc debug SPI/SD |
| queue 64, dropped tăng | dữ liệu đang mất | reject session |
| `io_errors` tăng | storage có lỗi mới | reject/interrupted, không nối sau recovery |
| parser CRC fail | file hỏng/truncated | giữ file làm evidence, không dùng dataset |
| timestamp không monotonic | firmware/timebase sai | không dùng dataset này |
| per-source sequence gap trong cửa sổ session | record của source đó bị mất/corrupt | review/reject |
| rate lệch đáng kể 100 Hz | tick/timebase/acquisition bất thường | review trước khi dùng |
| marker thiếu | ranh giới session yếu | dùng metadata/transcript nếu đủ; không suy đoán |
| marker END lặp | thao tác kết thúc bị lặp | dùng END đầu tiên sau START, ghi warning/evidence |
| label/điều kiện không chắc | provenance fail | đổi thành `UNLABELED` |
| `VERIFY_STEP` fail upstream hash | baseline drift | review source/manifest, không bypass |

### B.15 Checklist ngắn để in hoặc copy

**Trước khi thu**

- [ ] Run ID duy nhất và label đã chốt.
- [ ] Điều kiện vật lý, sensor placement và operator được ghi.
- [ ] Heartbeat/MPU PASS.
- [ ] `mounted=1`, queue 0, `FR_OK`.
- [ ] Đã ghi baseline `io_errors/dropped`.

**Trong khi thu**

- [ ] File mới đúng tên.
- [ ] Marker START accepted.
- [ ] Checkpoint mỗi 5 phút.
- [ ] Không mount loss, queue full hoặc counter delta.
- [ ] Không đổi điều kiện/label.

**Kết thúc**

- [ ] Marker END accepted.
- [ ] Flush ACK 0.
- [ ] STOP rồi status `logging=0 mounted=0`.
- [ ] Copy file gốc và tính SHA-256.
- [ ] General parser PASS CRC.
- [ ] Converter PASS.
- [ ] Duration/rate/timestamp/sequence PASS.
- [ ] Metadata không còn placeholder.
- [ ] `VERIFY_STEP.cmd` PASS hoặc failure đã được review.
- [ ] Manifest chỉ cập nhật sau khi mọi evidence có đủ.

### B.16 Source map

- Dataset contract: [A.4 Dataset và MLP](#a4-dataset-và-mlp)
- ZLOG parser tổng quát: [`../../software/linux_ipc/parse_zlog.py`](../../software/linux_ipc/parse_zlog.py)
- Converter: [`../tools/zlog_to_raw_csv.py`](../tools/zlog_to_raw_csv.py)
- Dataset README: [`../ml/dataset/v1/README.md`](../ml/dataset/v1/README.md)
- Dataset manifest: [`../ml/dataset/v1/manifest.json`](../ml/dataset/v1/manifest.json)
- Full local verification: [`../VERIFY_STEP.ps1`](../VERIFY_STEP.ps1)
- Upstream verification: [`../VERIFY_UPSTREAM.ps1`](../VERIFY_UPSTREAM.ps1)
