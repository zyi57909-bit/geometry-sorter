/*
 * ============================================================================
 *  AutoMeasureSort.ino —— 上电自动传送 → K230 视觉检测 → 分选
 *  2026-10-26：新增 Timer3 1ms 定时器中断采样传感器（文中用 //10.26添加中断 标出）
 *  传感器停带改为「检测到后延迟 0.5s」（文中用 //传送带停带延迟 标出）
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
int Out4 = A4;           //Out4（分选漏斗）

// 传感器 NPN 信号线引脚
const int SENSOR_PIN_1 = 44;   // 对应 Out1
const int SENSOR_PIN_2 = 45;   // 对应 Out2

const long BAUD_RATE = 115200;
const unsigned long DEBOUNCE_DELAY_MS = 200;

// 中断和主程序共享1 

//10.26添加中断：stop1/stop2 在 Timer3 中断里被写，必须加 volatile（中断与主程序共享）
volatile bool stop1 = false;   // 一级带停
volatile bool stop2 = false;   // 二级带停

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
#define Serial1_BAUD     115200   // Serial1(18/19) TJC 串口屏
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
判断状态决定输出
 *****************************************/

void applyConveyors() {
  digitalWrite(Out1, stop1 ? LOW : HIGH);
  digitalWrite(Out4, stop1 ? LOW : HIGH);
  digitalWrite(Out2, stop2 ? LOW : HIGH);
  digitalWrite(Out3, HIGH);      // 三级不参与停带
}

// ============================================================================
// 10.26添加中断：Timer3 每 1ms 采样一次 44/45 脚，检测到物体后延迟停带
//   为什么加：主循环里有 delay()，原来每圈 1.4~8s 才读一次传感器，而物体
//             经过传感器只有 0.1~0.3s，大部分时候都看不到 -> 传送带不会停。
//             定时器中断不依赖主循环，采样和停带都准时。
//   为什么判下降沿：分选完成重启传送带后，同一个物体可能还挡在传感器前，
//             如果按"低电平就停"会在重启后 1ms 内又停住，带子一步都走不了。
//             只认「高 -> 低」的跳变，就不会重复触发。
// ============================================================================

//传送带停带延迟：检测到物体后，带子继续走 STOP_DELAY_MS 毫秒再停（单位 ms，可改）
const unsigned int STOP_DELAY_MS = 500;

ISR(TIMER3_COMPA_vect) {
  static uint8_t  prev1 = HIGH, prev2 = HIGH;   // 上一次采样电平
  static uint16_t left1 = 0,    left2 = 0;      // 停带倒计时（ms），0 = 不在倒计时

  uint8_t r1 = digitalRead(SENSOR_PIN_1);
  if (prev1 == HIGH && r1 == LOW && left1 == 0) {
    left1 = STOP_DELAY_MS;        //传送带停带延迟：检测到物体1，开始 0.5s 倒计时
  } else if (left1 > 0 && --left1 == 0) {
    digitalWrite(Out1, LOW);      //传送带停带延迟：倒计时到，停 1 号传送带
    digitalWrite(Out4, LOW);      //与1号传送带同时停止
    stop1 = true;
  }
  prev1 = r1;

  uint8_t r2 = digitalRead(SENSOR_PIN_2);
  if (prev2 == HIGH && r2 == LOW && left2 == 0) {
    left2 = STOP_DELAY_MS;        //传送带停带延迟：检测到物体2，开始 0.5s 倒计时
  } else if (left2 > 0 && --left2 == 0) {
    digitalWrite(Out2, LOW);      //传送带停带延迟：倒计时到，停 2 号传送带
    stop2 = true;
  }
  prev2 = r2;
}


/****************************************
  映射表：13 色 × 34 形状 = 442 类（来源：对照表.xls）
  公式：颜色 = COLORS[code % 13]，形状 = SHAPES[code / 13]
 *****************************************/
const char* COLORS[13] = {"红","橙","黄","绿","青","蓝","紫","黑","白","灰","棕","卡其色","粉色"};
const char* SHAPES[34] = {
  "半圆","方管","胶囊体","六棱台","六棱柱","镂空正方体",
  "平行六面体","球体","三角筒","三棱台","三棱柱","三棱锥",
  "双锥体","1/4圆","四棱台","四棱锥","椭圆","五角管",
  "五棱台","五棱柱","五棱锥","斜三棱柱","圆环","圆台",
  "圆筒","圆柱体","圆锥体","长方体","正八面体","正方体","斜圆柱体",
  "五棱柱管","长方体柱管","六棱柱管"
};

// 代码 → 物体名称（如 91 → "红球体"）
void getObjectName(int code, char* name) {
  if (code < 0 || code >= 442) { strcpy(name, "未知"); return; }
  strcpy(name, COLORS[code % 13]);
  strcat(name, SHAPES[code / 13]);
}

/****************************************
  发送图片到 p<slot>（仅新物体调用，一物体一图）
 *****************************************/
void sendPic(int slot, int picId) {
  TJC.print("p");
  TJC.print(slot);
  TJC.print(".pic=");
  TJC.print(picId);
  SendEnd();
}

/****************************************
  HMI 上报：t0.txt = 物体名称；data0 = 序号^名称^数量
  isNew=true 时 insert 新行；false 时 up 更新原行数量
 *****************************************/
void sendRecord(int code, int count, int row, bool isNew) {
  char name[48];
  getObjectName(code, name);

  // t0.txt = 当前物体名称
  TJC.print("t0.txt=\"");
  TJC.print(name);
  TJC.print("\"");
  SendEnd();

  // data0 记录
  char content[64];
  snprintf(content, sizeof(content), "%d^%s^%d", row + 1, name, count);
  if (isNew) {
    TJC.print("data0.insert(\"");
    TJC.print(content);
    TJC.print("\")");
    SendEnd();
  } else {
    TJC.print("data0.up(\"");
    TJC.print(content);
    TJC.print("\",");
    TJC.print(row);
    TJC.print(")");
    SendEnd();
  }
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
  YmyStepper.setAcceleration(4200);
  YmyStepper.setSpeed(3200);
  
  XmyStepper.setCurrentPosition(0);  // 设置电机的当前位置为0
  YmyStepper.setCurrentPosition(850);
}

// ============================================================================
//  K230 回帧格式校验 
// 合法格式：Re,<标签数字>,<置信度>          例：Re,37,0.87
//   - 必须以 "Re," 开头（防止 "Ready..." 之类误判）
//   - 必须恰好两段：标签 + 置信度；缺段、多余逗号都判非法
//   - 标签必须是纯数字，且范围 0..441（13 色 × 34 形状 = 442 类）
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

  // 标签：纯数字 + 范围 0..441
  for (unsigned int i = 0; i < labelStr.length(); i++)
    if (!isDigit(labelStr[i])) return false;
  long l = labelStr.toInt();
  if (l < 0 || l > 441) return false;

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
      sendRecord(code, bucketCount[i], i, false);   //HMI 上报：data0 数量+1
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
      sendPic(i, code);                 //图片：p<槽位>.pic=代码（一物体一图）
      sendRecord(code, bucketCount[i], i, true);  //HMI 上报：data0 新增
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
  TJC.begin(115200);  // HMI通讯波特率为115200
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
  pinMode(Out4, OUTPUT);          // 设置端口为输出
  digitalWrite(Out1, LOW);
  digitalWrite(Out2, LOW);
  digitalWrite(Out3, LOW);
  digitalWrite(Out4, LOW);

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
  digitalWrite(Out4, HIGH);       //分选漏斗与启动传送带电机同步启动

  SendEnd();

  //10.26添加中断：Timer3 配成 1ms 触发一次的定时器中断
  //   16MHz / 8 分频 = 2MHz，计数 2000 次 = 1ms，到值就自动进入 TIMER3_COMPA_vect
  TCCR3A = 0;                        // 普通模式
  TCCR3B = _BV(WGM32) | _BV(CS11);   // CTC 模式 + 8 分频
  OCR3A  = 1999;                     // 1ms 触发一次（0..1999 共 2000 次）
  TIMSK3 |= _BV(OCIE3A);             // 打开比较匹配中断
  //  digitalWrite(Out2, HIGH);
}
/****************************************
  主程序
 *****************************************/
void loop() {

/****************************************
  传送带运行逻辑判断
 *****************************************/

//10.26添加中断：传感器已改由 Timer3 中断采样并直接停带（见 ISR(TIMER3_COMPA_vect)），
// 这里原来"读到低电平就停"的写法删除，避免重启后被同一个物体立刻又停住
applyConveyors();

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
    XmyStepper.runToNewPosition(0); //选位，小齿轮52，大齿轮104
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开,转过20齿，30齿为1600
    delay(2000); 
    YmyStepper.runToNewPosition(850); //虹膜关
    Category = 0;
  }
  /////////////种类2/////////////
  if (Category == 2) {
    XmyStepper.runToNewPosition(325); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(850); //虹膜关
    Category = 0;
  }
  /////////////种类3/////////////
  if (Category == 3) {
    XmyStepper.runToNewPosition(650); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(750); //虹膜关
    Category = 0;
  }
  /////////////种类4/////////////
  if (Category == 4) {
    XmyStepper.runToNewPosition(975); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(750); //虹膜关
    Category = 0;
  }

  /////////////种类5/////////////
  if (Category == 5) {
    XmyStepper.runToNewPosition(1300); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(750); //虹膜关
    Category = 0;
  }

  /////////////种类6/////////////
  if (Category == 6) {
    XmyStepper.runToNewPosition(1625); //选位
    delay(2000); 
    YmyStepper.runToNewPosition(0);//虹膜开
    delay(2000); 
    YmyStepper.runToNewPosition(750); //虹膜关
    Category = 0;
  }

///循环结判断是否启动
if (sortCat != 0) {
  stop1 = false;
  stop2 = false;
  applyConveyors();    // 三路一起启动
}
  delay(1000);

}
