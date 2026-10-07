// 内置工具 · tele：主机遥测（频率 / 温度 / 电池 / 性能模式）。
//
// ★ 这个工具之所以现在就能写，是因为 M1 探针在真机上实测证明
//   `clkrst` / `tc` / `i2c` / `psm` / `apm` 在 sysmodule 里**全部可用**
//   ——而开工单曾断言"时钟做不了"。
//
// 读数来源：
//   * 频率 —— `clkrst`（这是**系统请求值**，不是硬件瞬时值；后者要读 T210 寄存器，留待后续）
//   * 机身温度 —— `tc` 服务
//   * SOC / PCB 温度 —— I2C 直读 `tmp451`（与 sys-clk 同一颗传感器）
//   * 电池 —— `psm`
//   * 性能模式 —— `apm`
//
// 注：`soc_c` / `pcb_c` 取自 tmp451 的 reg0 / reg1。这个对应关系需要与 sys-clk 的覆盖层读数
// 在真机上对照一次再定案（先在 help 里写明，不假装已经确定）。
#include "nxc_sdk.hpp"

#include <switch.h>

namespace {

// ---------------------------------------------------------------- clkrst
bool readHz(PcvModuleId module, u32* out) {
    static bool ready = false;
    if (!ready) {
        if (R_FAILED(clkrstInitialize())) return false;
        ready = true;  // 常驻不退出：工具会在整个生命周期里反复用
    }
    ClkrstSession session;
    if (R_FAILED(clkrstOpenSession(&session, module, 0))) return false;
    const Result rc = clkrstGetClockRate(&session, out);
    clkrstCloseSession(&session);
    return R_SUCCEEDED(rc);
}

// ---------------------------------------------------------------- tmp451（I2C）
bool readTmp451(int reg, double* outCelsius) {
    static bool ready = false;
    if (!ready) {
        if (R_FAILED(i2cInitialize())) return false;
        ready = true;
    }
    I2cSession session;
    if (R_FAILED(i2cOpenSession(&session, I2cDevice_Tmp451))) return false;

    u8 command = static_cast<u8>(reg);
    u8 buf[2] = {0, 0};
    Result rc = i2csessionSendAuto(&session, &command, 1, I2cTransactionOption_All);
    if (R_SUCCEEDED(rc)) {
        rc = i2csessionReceiveAuto(&session, buf, sizeof(buf), I2cTransactionOption_All);
    }
    i2csessionClose(&session);
    if (R_FAILED(rc)) return false;

    // TMP451 格式：高字节是整数部分（有符号），低字节高 4 位是 1/16 度的小数。
    const int whole = static_cast<int>(static_cast<s8>(buf[0]));
    const int fraction = static_cast<int>((buf[1] >> 4) & 0x0F);
    *outCelsius = whole + fraction * 0.0625;
    return true;
}

// ---------------------------------------------------------------- 各条读数
void addFreq(nxc::Reply& r) {
    u32 hz = 0;
    if (readHz(PcvModuleId_CpuBus, &hz)) r.kvi("cpu_hz", hz); else r.kv("cpu_hz", "unavailable");
    if (readHz(PcvModuleId_GPU, &hz))    r.kvi("gpu_hz", hz); else r.kv("gpu_hz", "unavailable");
    if (readHz(PcvModuleId_EMC, &hz))    r.kvi("mem_hz", hz); else r.kv("mem_hz", "unavailable");
}

void addTemp(nxc::Reply& r) {
    if (R_SUCCEEDED(tcInitialize())) {
        s32 milli = 0;
        if (R_SUCCEEDED(tcGetSkinTemperatureMilliC(&milli))) r.kvi("skin_mc", milli);
        else r.kv("skin_mc", "unavailable");
        tcExit();
    } else {
        r.kv("skin_mc", "tc-unavailable");
    }

    double celsius = 0.0;
    if (readTmp451(0, &celsius)) r.kvf("soc_c", "%.2f", celsius);
    else r.kv("soc_c", "unavailable");
    if (readTmp451(1, &celsius)) r.kvf("pcb_c", "%.2f", celsius);
    else r.kv("pcb_c", "unavailable");
}

void addBattery(nxc::Reply& r) {
    if (R_FAILED(psmInitialize())) {
        r.kv("battery", "psm-unavailable");
        return;
    }
    u32 percent = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&percent))) r.kvi("battery_percent", percent);
    else r.kv("battery_percent", "unavailable");

    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger))) r.kvi("charger", static_cast<int>(charger));
    else r.kv("charger", "unavailable");

    bool enough = false;
    if (R_SUCCEEDED(psmIsEnoughPowerSupplied(&enough))) r.kvi("enough_power", enough ? 1 : 0);
    else r.kv("enough_power", "unavailable");

    PsmBatteryVoltageState voltage = PsmBatteryVoltageState_Normal;
    if (R_SUCCEEDED(psmGetBatteryVoltageState(&voltage))) r.kvi("voltage_state", static_cast<int>(voltage));
    else r.kv("voltage_state", "unavailable");

    psmExit();
}

void addPerformance(nxc::Reply& r) {
    if (R_FAILED(apmInitialize())) {
        r.kv("apm", "unavailable");
        return;
    }
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    if (R_SUCCEEDED(apmGetPerformanceMode(&mode))) {
        r.kvi("perf_mode", static_cast<int>(mode));
        r.kv("perf_mode_name",
             mode == ApmPerformanceMode_Boost ? "boost"
             : mode == ApmPerformanceMode_Normal ? "normal" : "invalid");
        u32 config = 0;
        if (R_SUCCEEDED(apmGetPerformanceConfiguration(mode, &config))) {
            r.kvf("perf_config", "0x%08X", config);
        }
    } else {
        r.kv("perf_mode", "unavailable");
    }
    apmExit();
}

// ---------------------------------------------------------------- 方法
void cmdFreq(const nxc::Args&, nxc::Reply& r) { addFreq(r); }
void cmdTemp(const nxc::Args&, nxc::Reply& r) { addTemp(r); }
void cmdBattery(const nxc::Args&, nxc::Reply& r) { addBattery(r); }
void cmdPerf(const nxc::Args&, nxc::Reply& r) { addPerformance(r); }

void cmdAll(const nxc::Args&, nxc::Reply& r) {
    addFreq(r);
    addTemp(r);
    addBattery(r);
    addPerformance(r);
}

// ---------------------------------------------------------------- 自描述元数据
//
// ★★ 规矩：**这个工具的全部知识都写在本文件里** —— 核心不认识 `tele` 是什么，
//    `tools.list` / `tools.doc` 只是把这里的内容汇总出去。**AI 就是靠这些字学会用它。**
//    ⇒ 新增/改动方法时，把参数表与教程一起改（别只改实现）。
// ★ tele.* 全是**只读**遥测（risk 一律 read）。
namespace {

const nxc::MethodInfo kInfoFreq = {
    nullptr, 0, "read",
    "读 CPU / GPU / MEM 的频率。\n"
    "作用: 看当前三个核心的时钟频率。\n"
    "★ 数据源是 clkrst 的**请求值** —— 是系统「要求」跑多少，**不是**硬件瞬时频率\n"
    "  （后者要读 T210 寄存器，暂未做）。\n"
    "参数: 无。\n"
    "示例: tele.freq\n"
    "返回: cpu_hz= gpu_hz= mem_hz=（单位 Hz；某项读不到时为 unavailable）。\n"
    "相关: clk（设/还原频率）、tele.perf。\n",
};
const nxc::MethodInfo kInfoTemp = {
    nullptr, 0, "read",
    "读温度：机身温度 + SoC/PCB 温度。\n"
    "作用: 看热状态。\n"
    "来源:\n"
    "  · skin_mc = 机身温度，走 tc 服务，单位**毫摄氏度**。\n"
    "  · soc_c / pcb_c = SoC / PCB 温度，I2C 直读 tmp451（与 sys-clk 同一颗传感器），单位摄氏度。\n"
    "★ tmp451 的 reg0→soc_c、reg1→pcb_c 这个对应关系还需与 sys-clk 覆盖层真机对照再定案\n"
    "  （先按这样写，不假装已经确定）。\n"
    "参数: 无。\n"
    "示例: tele.temp\n"
    "返回: skin_mc= soc_c= pcb_c=（读不到时为 unavailable / tc-unavailable）。\n"
    "相关: tele.perf（性能模式）。\n",
};
const nxc::MethodInfo kInfoBattery = {
    nullptr, 0, "read",
    "读电池与供电状态。\n"
    "作用: 看电量、有没有插充电器、供电够不够、电压状态。\n"
    "来源: psm 服务。\n"
    "参数: 无。\n"
    "示例: tele.battery\n"
    "返回:\n"
    "  battery_percent  电量百分比\n"
    "  charger          充电器类型（数值，对应 PsmChargerType）\n"
    "  enough_power     1=供电充足 0=不充足\n"
    "  voltage_state    电压状态（数值，对应 PsmBatteryVoltageState）\n"
    "  读不到时回 psm-unavailable / unavailable。\n"
    "相关: tele.perf。\n",
};
const nxc::MethodInfo kInfoPerf = {
    nullptr, 0, "read",
    "读性能模式。\n"
    "作用: 看主机当前是正常还是 Boost 模式，以及对应的性能配置号。\n"
    "来源: apm 服务。\n"
    "参数: 无。\n"
    "示例: tele.perf\n"
    "返回: perf_mode=（数值） perf_mode_name=boost|normal|invalid perf_config=0x……\n"
    "相关: clk（改频率）、tele.freq。\n",
};
const nxc::MethodInfo kInfoAll = {
    nullptr, 0, "read",
    "一次拿全套遥测 = freq + temp + battery + perf。\n"
    "作用: 一条命令看清主机状态，省往返。\n"
    "参数: 无。\n"
    "示例: tele.all\n"
    "返回: 上面四组字段的并集。\n"
    "相关: 上面四个单条命令。\n",
};

}  // namespace

const nxc::Method kMethods[] = {
    {"freq",    "CPU/GPU/MEM 频率（clkrst 的请求值）", cmdFreq,    &kInfoFreq},
    {"temp",    "机身温度（tc）+ SOC/PCB 温度（I2C 直读 tmp451）", cmdTemp, &kInfoTemp},
    {"battery", "电量百分比 / 充电器类型 / 供电是否充足 / 电压状态", cmdBattery, &kInfoBattery},
    {"perf",    "性能模式（normal/boost）与性能配置号", cmdPerf, &kInfoPerf},
    {"all",     "以上全部（一条命令拿全套遥测）", cmdAll, &kInfoAll},
};

}  // namespace

NXC_DEFINE_TOOL_EX(nxc_tool_tele, "tele", "主机遥测", kMethods,
    "主机只读遥测：频率 / 温度 / 电池 / 性能模式 —— 全是 read。\n"
    "各条:\n"
    "  · freq    CPU/GPU/MEM 频率（clkrst 的**请求值**，不是硬件瞬时值）\n"
    "  · temp    机身温度（tc）+ SoC/PCB 温度（I2C 直读 tmp451）\n"
    "  · battery 电量 / 充电器 / 供电是否充足 / 电压状态（psm）\n"
    "  · perf    性能模式（normal/boost）与性能配置号（apm）\n"
    "  · all     以上全套\n"
    "什么时候用它: 想知道机器热不热、频率被压到多少、还剩多少电时。\n"
    "边界: 全只读、不改任何状态；频率读的是请求值，别当实测频率用。\n"
    "相关工具: clk（改/还原频率）、power（电源）"
);
