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
 *
 *  ---------------------------------------------------------------------------
 *  [2026-10-21 本次改动] 只新增「K230 回帧格式校验」：
 *    - 新增 checkK230Frame()；loop 内两处解析改为先校验、后入桶
 *    - 代码中用 "★★★ [本次新增]" / "↓↓↓ [本次新增]" / "↑↑↑ [本次新增]" 标出
 *    - 其余逻辑、参数、HMI 上报均未改动
 *  ---------------------------------------------------------------------------
 * ============================================================================
 */
/****************************************
 智能分类硬件控制程序
 *****************************************/
#include <Arduino.h>
#include <SoftwareSerial.h>
#define TJC Serial1       //HMI接在串口1上
#define K230 Serial2      //K230接在串口2上
#include <AccelStepper.h>

//用于HMI和K230通信的接收变量
const byte NUM_BUCKETS = 6;    //桶的数量
bool learned[NUM_BUCKETS];       // 桶是否已占用
int  learnedCode[NUM_BUCKETS];   // 每个桶对应的物料码
byte bucketCount[NUM_BUCKETS];   // 每个桶当前物料数量
String dataRe = "";
byte Category = 0;       //类别

char str[100];           //用于向HMI传送数据

//用于Out控制的物理地址
int Out1 = A1;           //Out1
int Out2 = A2;           //Out2
int Out3 = A3;           //Out3

// 传感器 NPN 信号线引脚
const int SENSOR_PIN_1 = 44;   // 对应 Out1
const int SENSOR_PIN_2 = 45;   // 对应 Out2

const long BAUD_RATE = 115200;
const unsigned long DEBOUNCE_DELAY_MS = 200;

// 记录上一次状态，用于简单防抖
int lastState1 = HIGH;
int lastState2 = HIGH;

// 定义电机硬件接口
int MotorEn = 24;        //电机使能
int XdirPin = 26;  //X电机方向
int XstepPin = 10; //X电机脉冲
int YdirPin = 28;  //Y电机方向
int YstepPin = 11; //Y电机脉冲


// Define motor interface type and Creates an instance创建步进电机实例
#define motorInterfaceType 1
AccelStepper XmyStepper(motorInterfaceType, XstepPin, XdirPin);
AccelStepper YmyStepper(motorInterfaceType, YstepPin, YdirPin);
//AccelStepper ZmyStepper(motorInterfaceType, ZstepPin, ZdirPin);
//AccelStepper UmyStepper(motorInterfaceType, UstepPin, UdirPin);

// ===============================串口约束====================================
#define SERIAL_BAUD  115200   // Serial(USB) 调试 / 上报上位机
#define Serial1_BAUD     9600     // Serial1(18/19) TJC 串口屏
#define Serial2_BAUD  115200   // Serial2(16/17) K230

int a = 1;
unsigned long nowtime;    //用于记录时间，做延时用
/****************************************
  HMI 发送结速标志位
 *****************************************/
void SendEnd() {
  TJC.write(0xff);
  TJC.write(0xff);
  TJC.write(0xff);
}

/****************************************
10.22添加用于控制启停的函数（暂时不用）
 *****************************************/
bool stop1 = false;   // 一级带停
bool stop2 = false;   // 二级带停
//void setConveyors(bool run) {
  //digitalWrite(Out1, run ? HIGH : LOW);
  //digitalWrite(Out2, run ? HIGH : LOW);
  //digitalWrite(Out3, run ? HIGH : LOW);
//}



/****************************************
10.22添加按状态统一输出
 *****************************************/

void applyConveyors() {
  digitalWrite(Out1, stop1 ? LOW : HIGH);
  digitalWrite(Out2, stop2 ? LOW : HIGH);
  digitalWrite(Out3, HIGH);      // 三级不参与停带
}


/****************************************
  HMI 上报：t0.txt = 标签，t1.txt = 该桶数量
  （t0/t1 当前为文本控件，所以用 .txt=）
 *****************************************/
void HmiReport(int tag, byte count) {
  TJC.print("t0.txt=\"");
  TJC.print(tag);
  TJC.print("\"");
  SendEnd();

  TJC.print("t1.txt=\"");
  TJC.print((int)count);
  TJC.print("\"");
  SendEnd();
}
/****************************************
  电机初始化参数设置
 *****************************************/
void Motor_init() {
  pinMode(MotorEn, OUTPUT);          // 设置电机使能端口为输出
  digitalWrite(MotorEn, LOW);        //电机使能启动，要将其拉低才能使能，低电平有效
  XmyStepper.setMaxSpeed(6400);      // 设置X轴电机最大速度，加速度，运行速度
  XmyStepper.setAcceleration(3200);
  XmyStepper.setSpeed(3200);

  XmyStepper.setMinPulseWidth(20);
  YmyStepper.setMinPulseWidth(20);

  YmyStepper.setMaxSpeed(6400);     // 设置y轴电机最大速度，加速度，运行速度
  YmyStepper.setAcceleration(3200);
  YmyStepper.setSpeed(3200);
  XmyStepper.setCurrentPosition(0);  // 设置电机的当前位置为0
  YmyStepper.setCurrentPosition(0);
}

// ============================================================================
// ★★★ [本次新增] K230 回帧格式校验 ★★★
// 合法格式：Re,<标签数字>,<置信度>          例：Re,37,0.87
//   - 必须以 "Re," 开头（防止 "Ready..." 之类误判）
//   - 必须恰好两段：标签 + 置信度；缺段、多余逗号都判非法
//   - 标签必须是纯数字，且范围 0..402（13 色 × 31 形状 = 403 类）
//   - 置信度必须是 0.00~1.00 的数字，最多一个小数点
//   - 兼容外层括号：(Re,37,0.87)
// 返回 true 时 label / conf 有效；返回 false 时调用方丢弃该帧并打 [WARN]
// ============================================================================
bool checkK230Frame(String frame, int &label, float &conf) {
  frame.trim();

  // 兼容外层括号：(Re,37,0.87)
  if (frame.startsWith("(") && frame.endsWith(")")) {
    frame = frame.substring(1, frame.length() - 1);
    frame.trim();
  }

  // 前缀必须是 "Re,"（不是只判 "Re"）
  if (!frame.startsWith("Re,")) return false;

  // 必须恰好两段：Re,标签,置信度
  int p1 = frame.indexOf(',');
  int p2 = frame.indexOf(',', p1 + 1);
  if (p1 < 0 || p2 < 0) return false;
  if (frame.indexOf(',', p2 + 1) >= 0) return false;   // 多余逗号

  String labelStr = frame.substring(p1 + 1, p2);
  String confStr  = frame.substring(p2 + 1);
  labelStr.trim();
  confStr.trim();
  if (labelStr.length() == 0 || confStr.length() == 0) return false;

  // 标签：纯数字 + 范围 0..402
  for (unsigned int i = 0; i < labelStr.length(); i++)
    if (!isDigit(labelStr[i])) return false;
  long l = labelStr.toInt();
  if (l < 0 || l > 402) return false;

  // 置信度：数字 + 最多一个小数点 + 范围 0.00~1.00
  bool dot = false;
  for (unsigned int i = 0; i < confStr.length(); i++) {
    char c = confStr[i];
    if (c == '.') {
      if (dot) return false;          // 多个小数点
      dot = true;
    } else if (!isDigit(c)) {
      return false;                   // 非数字字符
    }
  }
  conf = confStr.toFloat();
  if (conf < 0.0 || conf > 1.0) return false;

  label = (int)l;
  return true;
}

//***********物料码转桶号函数************
void codeToBucket(int code) {
  // 第一步：查旧
  for (byte i = 0; i < NUM_BUCKETS; i++)
    if (learned[i] && learnedCode[i] == code) {
      bucketCount[i]++;                  // 数量+1
      Serial.print(i + 1);               // 输出桶号
      Serial.print(",数量");
      Serial.println(bucketCount[i]);    // 输出数量
      Category = i + 1;                  //输出桶号，用于控制分料斗转动
      HmiReport(code, bucketCount[i]);   //HMI 上报：标签 + 该桶数量
      return;                  // 结束
    }

  // 第二步：占新
  for (byte i = 0; i < NUM_BUCKETS; i++)
    if (!learned[i]) {
      learned[i]     = true;            // 标记占用
      learnedCode[i] = code;            // 记下物料码
      bucketCount[i]++;                // ★ 数量+1
      Serial.print(i + 1);              // 输出桶号
      Serial.print(",数量");
      Serial.println(bucketCount[i]);   // 输出数量
      Category = i + 1;                 //输出桶号，用于控制分料斗转动
      HmiReport(code, bucketCount[i]);  //HMI 上报：标签 + 该桶数量
      return;                           // 结束
    }
}

/****************************************
  初始化程序
 *****************************************/
void setup() {
  //串口波特率设置
  Serial.begin(115200);
  delay(50);
  TJC.begin(9600);    // HMI通讯波特率为9600
  delay(50);
  K230.begin(115200); //K230通讯波特率为115200
  delay(50);
  nowtime = millis(); // 记录当前CPU运行时间
  Motor_init();       //电机驱动初始化
  delay(50);

  //数字输出端口设置
  pinMode(Out1, OUTPUT);          // 设置端口为输出
  pinMode(Out2, OUTPUT);          // 设置端口为输出
  pinMode(Out3, OUTPUT);          // 设置端口为输出
  digitalWrite(Out1, LOW);
  digitalWrite(Out2, LOW);
  digitalWrite(Out3, LOW);

  // 传感器 NPN 开集输出，上拉
  pinMode(SENSOR_PIN_1, INPUT_PULLUP);
  pinMode(SENSOR_PIN_2, INPUT_PULLUP);

  Serial.begin(BAUD_RATE);

  for (byte i = 0; i < NUM_BUCKETS; i++) {  //初始化桶号，桶内存放物
    learned[i]     = false;      // 是否已占用清零
    learnedCode[i] = -1;         // 物料码清零
    bucketCount[i] = 0;          // 数量清零
  }


  delay(300);

//  delay(10000);                   //延时10秒等待K230开机
  digitalWrite(Out1, HIGH);       //启动传送带电机
  TJC.print("t0.txt=\"准备就绪\"");                    //向串口屏发送垃圾类别信息
  SendEnd();
  //  digitalWrite(Out2, HIGH);
}
/****************************************
  主程序
 *****************************************/
void loop() {

 // digitalWrite(Out1, HIGH);    // 启动所有电机
 // digitalWrite(Out2, HIGH);
 // digitalWrite(Out3, HIGH);

 //10.22新增条件判断是否启停
// if (!beltStopped) {
//  applyConveyors();     // 运行态保持启动；停带态不再每轮拉高
//}
/****************************************
  10.22新增传送带判断
 *****************************************/

int a1 = digitalRead(SENSOR_PIN_1);
if (a1 == LOW && lastState1 == HIGH) stop1 = true;
lastState1 = a1;

int a2 = digitalRead(SENSOR_PIN_2);
if (a2 == LOW && lastState2 == HIGH) stop2 = true;
lastState2 = a2;

applyConveyors();
 //////////////传感器检测程序（10.22将其禁用）/////////////
 // int s1 = digitalRead(SENSOR_PIN_1);
 // if (s1 == LOW && lastState1 == HIGH) {   // 从"无"到"有"的跳变
 //   digitalWrite(Out1, LOW);
 // }
 // lastState1 = s1;

  // ---- 传感器2：检测到物体就停 Out2 ----
 // int s2 = digitalRead(SENSOR_PIN_2);
 // if (s2 == LOW && lastState2 == HIGH) {
 //   digitalWrite(Out2, LOW);
 // }
 // lastState2 = s2;

 // delay(DEBOUNCE_DELAY_MS);   // 简单防抖

 ///////////////////////////

  if (Serial.available() > 0) {          // 有数据才处理
    dataRe = Serial.readString();        // 读整串，如 "Re,45,0.87\r\n"         
    dataRe.trim();                       // 去掉首尾空格/\r/\n
    // ↓↓↓ [本次新增] 先用 checkK230Frame() 校验帧格式，合法才入桶 ↓↓↓
    if (dataRe.length() > 0) {
      int label = -1;
      float conf = 0.0;
      if (checkK230Frame(dataRe, label, conf)) {
        codeToBucket(label);             // 校验通过：转 int，交给核心函数
      } else {
        Serial.print("[WARN] K230 帧格式不合法: ");
        Serial.println(dataRe);
      }
    }
    // ↑↑↑ [本次新增] ↑↑↑
  }

  K230.print("image");                   // 发送image，启动K230图像检测
  delay(400);                            // 等待数据发送完成，且K230作出响应

  if (K230.available() > 0) {            // 有数据才处理
    dataRe = Serial2.readString();        // 读整串（注意：此处仍按你的版本读 Serial）
    dataRe.trim();                       // 去掉首尾空格/\r/\n
    // ↓↓↓ [本次新增] 先用 checkK230Frame() 校验帧格式，合法才入桶 ↓↓↓
    if (dataRe.length() > 0) {
      int label = -1;
      float conf = 0.0;
      if (checkK230Frame(dataRe, label, conf)) {
        codeToBucket(label);             // 校验通过：转 int，交给核心函数
      } else {
        Serial.print("[WARN] K230 帧格式不合法: ");
        Serial.println(dataRe);
      }
    }
    // ↑↑↑ [本次新增] ↑↑↑
  }


  //////////////种类1/////////////
  byte sortCat = Category;            //10.22加的代码
  if (Category == 1) {
//    digitalWrite(Out1, LOW);          // 关闭传送带电机
//    digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(0); //选位，小齿轮52，大齿轮104
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开,转过20齿，30齿为1600
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
 //   digitalWrite(Out1, HIGH);          //启动传送带电机
  }
  /////////////种类2/////////////
  if (Category == 2) {
//    digitalWrite(Out1, LOW);          // 关闭传送带电机
//    digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(267); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
 //   digitalWrite(Out1, HIGH);          //启动传送带电机
  }
  /////////////种类3/////////////
  if (Category == 3) {
//    digitalWrite(Out1, LOW);          // 关闭传送带电机
//    digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(533); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
  //  digitalWrite(Out1, HIGH);          //启动传送带电机
  }
  /////////////种类4/////////////
  if (Category == 4) {
//  digitalWrite(Out1, LOW);          // 关闭传送带电机
//  digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(800); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
 //   digitalWrite(Out1, HIGH);          //启动传送带电机
  }

  /////////////种类5/////////////
  if (Category == 5) {
//  digitalWrite(Out1, LOW);          // 关闭传送带电机
//  digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(1066); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
 //   digitalWrite(Out1, HIGH);          //启动传送带电机
  }

  /////////////种类6/////////////
  if (Category == 6) {
//    digitalWrite(Out1, LOW);          // 关闭传送带电机
//    digitalWrite(Out2, LOW);
    //    TJC.print(str);                   //向串口屏发送垃圾类别信息
    //    SendEnd();
    XmyStepper.runToNewPosition(1333); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(750);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(0); //虹膜关
    Category = 0;
//    digitalWrite(Out1, HIGH);          //启动传送带电机
  }

///10.22新增修改内容
if (sortCat != 0) {
  stop1 = false;
  stop2 = false;
  applyConveyors();    // 三路一起启动
}
  delay(1000);

}
