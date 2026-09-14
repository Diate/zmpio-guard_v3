# ZMPIO Guard V3 — DSP host simulation (gate Step1)

Thư mục này (`dsp_host_sim/`) là một snapshot độc lập ứng với gate **Step1** trong
`STEP_WORKSPACE.md`. Nó không sửa firmware CPU1, Linux, Vivado block design hay ABI IPC baseline.
Mục tiêu là khóa contract DSP và chạy được FIR/FFT/feature extraction trên host trước khi tích hợp
phần cứng ở Step2 (`hardware/rtl/`).

## Build nhanh trên máy hiện tại

```powershell
.\BUILD_STEP.cmd
```

Script tạo `build/`, build trực tiếp bằng MinGW `g++`, chạy regression và sinh
`golden/golden_tone_features.csv`. Cách này tương thích với MinGW 6.3 hiện có. CMake vẫn được cung
cấp cho máy có toolchain mới hơn:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
.\build\step1_golden.exe .\golden\golden_tone_features.csv
```

## Nội dung

Kiến trúc/contract hiện hành (input/output ABI, thuật toán DSP, streaming) nằm ở
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Chi tiết implementation, build/verification,
dataset/MLP, trạng thái gate và runbook thu ZLOG thật trên board nằm ở
[`docs/IMPLEMENTATION_AND_DEPLOYMENT.md`](docs/IMPLEMENTATION_AND_DEPLOYMENT.md).

- `fpga/dsp_core/src/`: mã C++ fixed-point, viết theo kiểu có thể chuyển sang Vitis HLS ở bước sau.
- `fpga/dsp_core/tb/`: regression host, gồm impulse/random FIR, FFT tone, feature contract và
  random back-pressure.
- `fpga/dsp_core/coefficients/`: coefficient và scale schedule được version hóa.
- `tools/`: chuyển ZLOG sang raw CSV và wrapper sinh golden vectors.
- `ml/dataset/v1/`: schema/manifest dataset; không tạo dữ liệu giả để qua gate.
- `ml/models/v1/`: MLP float 10→16→8→1 dùng Python standard library.
- `config/upstream_manifest.json`: dấu vết các file baseline đã dùng để chốt contract.

Kiểm tra xem baseline upstream còn đúng với lúc contract được chốt hay không:

```powershell
.\VERIFY_UPSTREAM.cmd
```

Chạy verification đầy đủ Debug + Release + golden + Python tools:

```powershell
.\VERIFY_STEP.cmd
```

## Trạng thái gate

Toàn bộ gate Step1 (host regression FIR/FFT/feature, dataset thật, MLP vs rule, Vitis HLS
resource/timing) đã **PASS** — bằng chứng chi tiết ở
[`docs/IMPLEMENTATION_AND_DEPLOYMENT.md`](docs/IMPLEMENTATION_AND_DEPLOYMENT.md#a5-trạng-thái-gate--bằng-chứng).
Nguồn sự thật cấp cao nhất cho trạng thái toàn dự án vẫn là
[`../docs/README.md`](../docs/README.md) và `CLAUDE.md` — khi tài liệu này mâu thuẫn với đó, tin
tài liệu ở root.
