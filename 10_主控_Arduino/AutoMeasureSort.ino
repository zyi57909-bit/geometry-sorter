/*
 * ============================================================================
 *  AutoMeasureSort.ino —— 上电自动传送 → K230 视觉检测 → 分选
 * ============================================================================
 *  板级框架基于学长已验证的 Hardware_control.ino（同型号主控板）；主流程采用
 *  非阻塞状态机（方案 B）。决策见 docs/decisions-measurement-flow.md。
 *
 *  流程（一个周期）：
 *    [上电]     立即启动三级传送带
 *    [RUN]      周期向 K230 发 "image"；物体进入视野（=到达测量工位）即被检测到
 *    [DETECTED] 收到 "标签 置信度" → 停三级传送带 → HMI 显示 + USB 串口上报
 *    [SORT]     分选电机转到位置 → [RELEASE] 开合电机打开 80°
 *    [DELAY]    落料停留 → [RESET] 开合关闭 + 分选复位 → 重启传送带 → 循环
 *
 *  关键决策：
 *    - 到位检测 = K230 视觉即到位（硬件无超声/无到位传感器）
 *    - K230 侧已改为「未检测到不返回」，只有确认检测到物体才回 "标签 置信度"
 *    - 分选 = 分选电机 6 位置 + 开合电机 80°（闭环 3200 脉冲/圈）
 *    - 上报上位机 = USB Serial + HMI 屏（不做网络）
 *
 *  依赖：AccelStepper 库。
 * ============================================================================
 */

#include <AccelStepper.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "diaglog.h"

// ============================================================================
// 一、串口
// ============================================================================
#define SERIAL_BAUD  115200   // Serial(USB) 调试 / 上报上位机
#define HMI_BAUD     9600     // Serial1(18/19) TJC 串口屏
#define VISION_BAUD  115200   // Serial2(16/17) K230

// ============================================================================
// 二、引脚（与 Hardware_control.ino 一致）
// ============================================================================
#define OUT1_PIN A1           // 1 号传送带
#define OUT2_PIN A2           // 2 号传送带
#define OUT3_PIN A3           // 3 号传送带
#define X_STEP_PIN 10         // 分选电机 脉冲
#define X_DIR_PIN  26         // 分选电机 方向
#define Y_STEP_PIN 11         // 开合电机 脉冲
#define Y_DIR_PIN  28         // 开合电机 方向
#define MOTOR_EN_PIN 24       // 电机使能（低有效）

// ============================================================================
// 三、电机参数（闭环步进，16 细分 = 3200 脉冲/圈）
// ============================================================================
#define MOTOR_INTERFACE_TYPE 1
const long  SORTER_STEPS_PER_REV  = 3200;
const int   SORTER_POSITION_COUNT = 6;
const float SORTER_SPEED          = 300;
const float SORTER_ACCEL          = 300;
const long  GATE_STEPS_PER_REV    = 3200;
const float GATE_OPEN_DEG         = 80.0;
const long  GATE_OPEN_STEPS       = (long)(GATE_OPEN_DEG / 360.0 * GATE_STEPS_PER_REV + 0.5);
const float GATE_SPEED            = 400;
const float GATE_ACCEL            = 400;
const unsigned int MIN_PULSE_US   = 20;

// ============================================================================
// 四、时序（毫秒）
// ============================================================================
const unsigned long VISION_WAIT_MS   = 500;    // 发 image 后等 K230 结果的超时，超时重发
const unsigned long RELEASE_DELAY_MS = 1000;   // 开合打开后落料停留
const int           RETRY_MAX        = 5;      // A4：同一目标最多重试 5 次（收 NONE 或超时各算一次），之后记 MISS 放行

// ============================================================================
// 五、K230 / HMI 协议
// ============================================================================
const char* VISION_TRIGGER = "image";
#define VISION_FRAME_MAX 48
#define TAG_MAX_LEN      16
#define HMI_TEXT_CTRL  "t0"   // 文本控件
#define HMI_COLOR_CTRL "t1"   // 颜色编号（数字控件）
#define HMI_SHAPE_CTRL "t2"   // 形状编号（数字控件）
// 对照表规模（颜色 13 × 形状 31 = 403），用于「标签ID → 颜色/形状编号」
const int COLOR_COUNT  = 13;
const int SHAPE_COUNT  = 31;
const int OBJECT_COUNT = COLOR_COUNT * SHAPE_COUNT;  // 403

// ============================================================================
// 六、对象与状态
// ============================================================================
AccelStepper sorter(MOTOR_INTERFACE_TYPE, X_STEP_PIN, X_DIR_PIN);
AccelStepper gate(MOTOR_INTERFACE_TYPE, Y_STEP_PIN, Y_DIR_PIN);

enum State { RUN = 0, DETECTED = 1, SORT = 2, RELEASE = 3, DELAY = 4, RESET = 5 };
State state = RUN;
bool  entered = true;
unsigned long stateStartMs = 0;
unsigned long delayStartMs = 0;
int   retryCount = 0;          // 连续未识别/超时次数，识别成功即清零

static char  vLabel[TAG_MAX_LEN];
static float vConf = 0.0f;
static int   targetPos = 0;

// ============================================================================
// 七、日志（USB 串口）
// ============================================================================
static void logMsg(const char* level, const char* fmt, ...) {
  char buf[110];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  Serial.print("["); Serial.print(level); Serial.print("] ");
  Serial.print(millis()); Serial.print("ms "); Serial.println(buf);
}
#define LOG_INFO(...)  logMsg("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  logMsg("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) logMsg("ERROR", __VA_ARGS__)

// ============================================================================
// 八、传送带（三级）
// ============================================================================
static uint8_t levelPin(int level) {
  if (level == 1) return OUT1_PIN;
  if (level == 2) return OUT2_PIN;
  if (level == 3) return OUT3_PIN;
  return 0xFF;
}
static void conveyorStart(int level) {
  uint8_t p = levelPin(level); if (p == 0xFF) return;
  digitalWrite(p, HIGH);
  LOG_INFO("传送带 L%d 启动", level);
  diagConv((uint8_t)level, true);
}
static void conveyorStop(int level) {
  uint8_t p = levelPin(level); if (p == 0xFF) return;
  digitalWrite(p, LOW);
  LOG_INFO("传送带 L%d 停止", level);
  diagConv((uint8_t)level, false);
}
static void conveyorStartAll() { for (int i = 1; i <= 3; i++) conveyorStart(i); }
static void conveyorStopAll()  { for (int i = 1; i <= 3; i++) conveyorStop(i); }

// ============================================================================
// 九、电机（分选 / 开合）
// ============================================================================
static long sorterPosToSteps(int pos) { return (long)pos * SORTER_STEPS_PER_REV / SORTER_POSITION_COUNT; }
static void sorterMoveTo(int pos) {
  long steps = sorterPosToSteps(pos);
  LOG_INFO("分选电机 -> 位置 %d（%ld 脉冲）", pos, steps);
  diagSorter((uint8_t)pos, (uint16_t)steps);
  sorter.moveTo(steps);
}
static void sorterHome() { sorter.moveTo(0); }
static bool sorterBusy() { return sorter.distanceToGo() != 0; }
static void gateOpen() {
  LOG_INFO("开合电机 打开（%ld 脉冲）", (long)GATE_OPEN_STEPS);
  diagGate(0, (uint16_t)GATE_OPEN_STEPS);
  gate.moveTo(GATE_OPEN_STEPS);
}
static void gateClose() {
  diagGate(1, 0);
  gate.moveTo(0);
}
static bool gateBusy() { return gate.distanceToGo() != 0; }
static void motorEnable(bool en) { digitalWrite(MOTOR_EN_PIN, en ? LOW : HIGH); }

// ============================================================================
// 十、K230 视觉（非阻塞收帧）
// ============================================================================
static char    vbuf[VISION_FRAME_MAX];
static uint8_t vlen = 0;
static unsigned long lastByteMs = 0;

static void visionRequest() {
  vlen = 0; vbuf[0] = '\0'; lastByteMs = millis();
  while (Serial2.available() > 0) Serial2.read();   // 清空残留旧帧
  Serial2.print(VISION_TRIGGER);
}

static int finishFrame() {
  int n = strlen(vbuf);
  while (n > 0 && (vbuf[n-1] == '\r' || vbuf[n-1] == '\n' || vbuf[n-1] == ' ')) vbuf[--n] = '\0';
  char* p = vbuf; while (*p == ' ') p++;
  if (*p == '\0') { vlen = 0; vbuf[0] = '\0'; return -1; }
  if (strncmp(p, "No objects", 10) == 0) { vlen = 0; vbuf[0] = '\0'; return 0; }
  // 接口约定：K230 未识别时回 "NONE\n"。必须在此拦掉——否则它会被当成正常标签
  // （vLabel="NONE"、置信度默认 1.0 → 停带 + 随机落料，等于静默误动作）。
  if (strncmp(p, "NONE", 4) == 0) { vlen = 0; vbuf[0] = '\0'; return 0; }

  vConf = 1.0f;
  char* sp = strchr(p, ' ');
  if (sp != NULL) {
    *sp = '\0';
    const char* c = sp + 1; while (*c == ' ') c++;
    if (*c != '\0') vConf = (float)atof(c);
  }
  strncpy(vLabel, p, TAG_MAX_LEN - 1); vLabel[TAG_MAX_LEN - 1] = '\0';
  vlen = 0; vbuf[0] = '\0';
  return 1;
}

static int visionPoll() {
  bool gotByte = false;
  while (Serial2.available() > 0) {
    char c = (char)Serial2.read();
    gotByte = true;
    if (vlen < VISION_FRAME_MAX - 1) { vbuf[vlen++] = c; vbuf[vlen] = '\0'; }
    lastByteMs = millis();
    if (c == '\n') return finishFrame();
  }
  if (vlen > 0 && !gotByte && (millis() - lastByteMs) >= 20) return finishFrame();
  return -1;
}

// ============================================================================
// 十一、HMI（TJC，指令 + 0xFF×3 结束符）
// ============================================================================
static void hmiSend(const char* cmd) {
  Serial1.print(cmd);
  Serial1.write(0xFF); Serial1.write(0xFF); Serial1.write(0xFF);
}
static void hmiShowText(const char* ctrl, const char* text) {
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "%s.txt=\"%s\"", ctrl, text);
  hmiSend(cmd);
}
static void hmiShowNum(const char* ctrl, int v) {
  char cmd[32];
  snprintf(cmd, sizeof(cmd), "%s.val=%d", ctrl, v);
  hmiSend(cmd);
}
// 标签是否全为数字；是则返回其数值，否则返回 -1
static int labelToId(const char* s) {
  if (s == NULL || *s == 0) return -1;
  for (const char* q = s; *q; q++) { if (*q < '0' || *q > '9') return -1; }
  return atoi(s);
}

// ============================================================================
// 十二、状态机
// ============================================================================
static void setState(State s) {
  if (state != s) { diagState((uint8_t)state, (uint8_t)s); state = s; entered = true; }
}
static const char* stateName(State s) {
  switch (s) {
    case RUN:      return "RUN";
    case DETECTED: return "DETECTED";
    case SORT:     return "SORT";
    case RELEASE:  return "RELEASE";
    case DELAY:    return "DELAY";
    case RESET:    return "RESET";
  }
  return "?";
}

static void updateRun() {
  if (entered) { entered = false; stateStartMs = millis(); visionRequest(); }
  int r = visionPoll();
  if (r == 1) {
    conveyorStopAll();
    retryCount = 0;   // 识别成功，计数清零
    LOG_INFO("检测到目标: 标签=%s 置信度=%d%% → 停传送带", vLabel, (int)(vConf * 100));
    diagK230(true, (uint8_t)(atoi(vLabel) & 0xFF), (uint16_t)(vConf * 100));
    setState(DETECTED);
  } else if (r == 0) {
    // K230 明确回 NONE：空载/漏检。不停带，计入重试；满 5 次记 MISS 后放行。
    retryCount++;
    if (retryCount >= RETRY_MAX) {
      LOG_WARN("MISS: 连续 %d 次未识别，放行并继续送带", retryCount);
      retryCount = 0;
    } else {
      LOG_INFO("未识别 NONE（%d/%d），继续送带", retryCount, RETRY_MAX);
    }
    visionRequest(); stateStartMs = millis();
  } else if (millis() - stateStartMs >= VISION_WAIT_MS) {
    retryCount++;   // 超时也计一次失败
    if (retryCount >= RETRY_MAX) {
      LOG_WARN("MISS: 连续 %d 次等待超时，放行并继续", retryCount);
      retryCount = 0;
    } else {
      LOG_WARN("等待 K230 超时（%d/%d），重发 image", retryCount, RETRY_MAX);
    }
    visionRequest(); stateStartMs = millis();
  }
}

static void updateDetected() {
  if (entered) {
    entered = false;
    // 上报上位机（USB 串口）
    Serial.print("RESULT:"); Serial.print(vLabel); Serial.print(","); Serial.println(vConf, 2);
    // HMI 显示（沿用 GeometricSortingTest.ino 格式：t0.txt + t1.val / t2.val）
    int id = labelToId(vLabel);
    if (id >= 0 && id < OBJECT_COUNT) {
      int color = id % COLOR_COUNT + 1;
      int shape = id / COLOR_COUNT + 1;
      hmiShowNum(HMI_COLOR_CTRL, color);
      hmiShowNum(HMI_SHAPE_CTRL, shape);
      LOG_INFO("HMI: 颜色编号=%d 形状编号=%d", color, shape);
    } else {
      LOG_WARN("HMI: 标签 [%s] 非数字，无法映射编号", vLabel);
      hmiShowText(HMI_TEXT_CTRL, "DETECT");
    }
    // 选分选位置（当前为随机 6 选 1，类别→盒位映射待定）
    targetPos = random(SORTER_POSITION_COUNT);
    sorterMoveTo(targetPos);
    setState(SORT);
  }
}

static void updateSort() { if (!sorterBusy()) setState(RELEASE); }

static void updateRelease() {
  if (entered) { entered = false; gateOpen(); }
  if (!gateBusy()) { delayStartMs = millis(); setState(DELAY); }
}

static void updateDelay() { if (millis() - delayStartMs >= RELEASE_DELAY_MS) setState(RESET); }

static void updateReset() {
  if (entered) { entered = false; gateClose(); sorterHome(); LOG_INFO("开合关闭 + 分选复位"); }
  if (!gateBusy() && !sorterBusy()) {
    diagCycle();
    conveyorStartAll();
    LOG_INFO("复位完成，重启三级传送带，进入下一轮");
    setState(RUN);
  }
}

// ============================================================================
// 十三、setup / loop
// ============================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial1.begin(HMI_BAUD);
  Serial2.begin(VISION_BAUD);
  diagInit();

  randomSeed(analogRead(A0) + (unsigned long)millis());

  pinMode(OUT1_PIN, OUTPUT); digitalWrite(OUT1_PIN, LOW);
  pinMode(OUT2_PIN, OUTPUT); digitalWrite(OUT2_PIN, LOW);
  pinMode(OUT3_PIN, OUTPUT); digitalWrite(OUT3_PIN, LOW);
  pinMode(X_STEP_PIN, OUTPUT); pinMode(X_DIR_PIN, OUTPUT);
  pinMode(Y_STEP_PIN, OUTPUT); pinMode(Y_DIR_PIN, OUTPUT);
  pinMode(MOTOR_EN_PIN, OUTPUT);

  motorEnable(true);
  sorter.setMaxSpeed(SORTER_SPEED); sorter.setAcceleration(SORTER_ACCEL);
  sorter.setMinPulseWidth(MIN_PULSE_US); sorter.setCurrentPosition(0);
  gate.setMaxSpeed(GATE_SPEED); gate.setAcceleration(GATE_ACCEL);
  gate.setMinPulseWidth(MIN_PULSE_US); gate.setCurrentPosition(0);

  LOG_INFO("==== AutoMeasureSort 启动（上电自动传送→检测→分选）====");
  conveyorStartAll();          // 上电立即启动三级传送带
  state = RUN;
  entered = true;
}

void loop() {
  // 串口命令（仅事后接电脑回读诊断日志）：d=dump, c=clear
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') diagDump();
    else if (c == 'c' || c == 'C') diagClear();
  }

  sorter.run();
  gate.run();

  State prev = state;
  switch (state) {
    case RUN:      updateRun();      break;
    case DETECTED: updateDetected(); break;
    case SORT:     updateSort();     break;
    case RELEASE:  updateRelease();  break;
    case DELAY:    updateDelay();    break;
    case RESET:    updateReset();    break;
  }
  if (prev != state) LOG_INFO("状态迁移: %s -> %s", stateName(prev), stateName(state));
}
