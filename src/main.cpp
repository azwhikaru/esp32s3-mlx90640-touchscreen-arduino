/*
 Huge thank you to Uspizig for offering this library that I have modified for TFT_eSPI: https://github.com/Uspizig/MLX90640
 Has to be Library, TFT_eSPI Rev 2.5.43
 The latest does not work

*/

#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"
#include "CST816T.h"
#include "BilinearInterpolation.h"
#include "kalman_filter.h"
// #include <WiFi.h>
// #include "esp_timer.h"
// #include "img_converters.h"
// #include "esp_http_server.h"

#define TA_SHIFT 8 // Default shift for MLX90640 in open air
#define MLX_VDD 46
#define MLX_SDA 41
#define MLX_SCL 42
#define TOUCH_SDA 2
#define TOUCH_SCL 3
#define TOUCH_RST 4

#define SCREEN_ROTATION 1
#define BAT_ADC 10
#define SCREEN_BL_PIN 8

#define _SCALE 9
#define BTN_LONG_PUSH_T 1000
#define TOUCH_LONG_PUSH_T 200
#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP 5           /* Time ESP32 will go to sleep (in seconds) */
#define GRIDCOLOR 0xA815

#define SEND_TO_SERIAL

#define INFO_BAR_HEIGHT 30

const int buttonPin1 = 0;
const int buttonPin2 = 21;

bool buttonState1 = 1;
bool buttonState2 = 1;

const byte MLX90640_address = 0x33;
static float mlx90640To[768]; // 从MLX90640读取的温度数据

// static float mlx90640To_buffer[768];       // 缓存区域，复制MLX90640读取的温度数据并用于绘制热力图
static int mlx90640To_buffer[768]; // 缓存区域，复制MLX90640读取的温度数据并用于绘制热力图

#if defined(SEND_TO_SERIAL)
static float mlx90640To_send_buffer[768]; // 缓存区域，复制MLX90640读取的温度数据，用于发送到上位机
static uint8_t *mlx90640To_Serial_buffer = (uint8_t *)mlx90640To_send_buffer;
#endif

static KFPTypeS kfpVar3Array[768]; // 卡尔曼滤波器变量数组

paramsMLX90640 mlx90640;
uint16_t test_points[5][2];
int brightness = 512;

int R_colour, G_colour, B_colour;
// int i, j;
float T_max, T_min, T_avg;
float T_center;
int max_x, max_y, min_x, min_y;

float bat_v;

bool lock = false; // 简单的锁，防止拷贝温度数据的时候对内存的访问冲突
bool touch_lock = false;

bool power_on = true;             // 是否开机
bool freeze = false;              // 暂停画面
bool show_local_temp_flag = true; // 是否显示点测温
bool clear_local_temp = false;    // 点测温清除

TFT_eSPI tft = TFT_eSPI();
CST816T touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, -1); // sda, scl, rst, irq

int diffx, diffy;

// 初始化卡尔曼滤波器数组的函数
const static float init_P = 0.1;
const static float init_G = 0.0;
const static float init_O = 26;
void KalmanArrayInit()
{
   // 循环遍历数组中的每个元素
   for (int i = 0; i < 768; ++i)
   {
      // 初始化每个元素
      kfpVar3Array[i] = (KFPTypeS){
          init_P, // 估算协方差. 初始化值为 0.02
          init_G, // 卡尔曼增益. 初始化值为 0
          init_O  // 卡尔曼滤波器输出. 初始化值为 0
      };
   }
}

// ===============================
// ===== determine the colour ====
// ===============================
void getColour(int j)
{
   if (j >= 0 && j < 30)
   {
      R_colour = 0;
      G_colour = 0;
      B_colour = 20 + 4 * j;
   }

   if (j >= 30 && j < 60)
   {
      R_colour = 4 * (j - 30);
      G_colour = 0;
      B_colour = 140 - 2 * (j - 30);
   }

   if (j >= 60 && j < 90)
   {
      R_colour = 120 + 4 * (j - 60);
      G_colour = 0;
      B_colour = 80 - 2 * (j - 60);
   }

   if (j >= 90 && j < 120)
   {
      R_colour = 255;
      G_colour = 0 + 2 * (j - 90);
      B_colour = 10 - (j - 90) / 3;
   }

   if (j >= 120 && j < 150)
   {
      R_colour = 255;
      G_colour = 60 + 175 * (j - 120) / 30;
      B_colour = 0;
   }

   if (j >= 150 && j <= 180)
   {
      R_colour = 255;
      G_colour = 235 + (j - 150) * 20 / 30;
      B_colour = 0 + 85 * (j - 150) / 10;
   }
}

void print_bitmap(int x, int y, uint16_t *data)
{

   for (int i = 0; i < y; i++)
   {
      for (int j = 0; j < x; j++)
      {
         Serial.print(data[i * j]);
         Serial.print(" ");
      }
      Serial.print("\n");
   }
   Serial.print("\n\n");
}

// Returns true if the MLX90640 is detected on the I2C bus
boolean isConnected()
{
   Wire.beginTransmission((uint8_t)MLX90640_address);
   if (Wire.endTransmission() != 0)
   {
      return (false);
   }
   return (true);
}

// 绘制十字
void draw_cross(int x, int y, int len, uint16_t color = tft.color565(255, 255, 255), int thickness = 1)
{
   if (!(y < tft.height() - INFO_BAR_HEIGHT)) {
      return;
   }

   // 绘制外层十字，增加线条宽度
   for (int i = 0; i < thickness; i++) {
      tft.drawLine(x - len / 2, y + i, x + len / 2, y + i, color); // 水平线
      tft.drawLine(x + i, y - len / 2, x + i, y + len / 2, color); // 垂直线
   }
}

// 点测温功能
void show_local_temp(int x, int y)
{
   draw_cross(x, y, 20);
   float temp_xy = mlx90640To[(24 - y / _SCALE) * 32 + (x / _SCALE)];
   int shift_x, shift_y;
   if (x < 140)
   {
      shift_x = 10;
   }
   else
   {
      shift_x = -40;
   }
   if (y < 120)
   {
      shift_y = 10;
   }
   else
   {
      shift_y = -10;
   }
   tft.setCursor(x + shift_x, y + shift_y);
   tft.printf("%.2f", temp_xy);
}

// void draw_heat_image(bool re_mapcolor=true){
//    tft.setRotation(3);
//    for (int i = 0 ; i < 24 ; i++){
//    for (int j = 0; j < 32; j++){
//       if (re_mapcolor) {mlx90640To_buffer[i*32 + j] = 180.0 * (mlx90640To_buffer[i*32 + j] - T_min) / (T_max - T_min);}
//       getColour(mlx90640To_buffer[i*32 + j]);
//       tft.fillRect(280 - j * _SCALE, (240 - _SCALE * 24) + i * _SCALE, _SCALE, _SCALE, tft.color565(R_colour, G_colour, B_colour));
//       // tft.drawBitmap();
//       }
//    }
//    tft.setRotation(SCREEN_ROTATION);
// }

// 在一块图像内存上绘制一个同色的方块
void draw_block_bitmap(int x, int y, int w, int max_x, int max_y, uint16_t color, uint16_t *data)
{
   for (int i = y; i < y + w; i++)
   {
      for (int j = x; j < x + w; j++)
      {
         data[i * max_x + j] = color;
      }
   }
}

const int lines = 10;
uint16_t lineBuffer[32 * _SCALE * lines]; // Toggle buffer for lines
uint16_t dmaBuffer1[32 * _SCALE * lines]; // Toggle buffer for lines
uint16_t dmaBuffer2[32 * _SCALE * lines]; // Toggle buffer for lines
uint16_t *dmaBufferPtr = dmaBuffer1;
bool dmaBufferSel = 0;
// 在屏幕上绘制热力图
void draw_heat_image_dma(bool re_mapcolor = true)
{
   static int value;
   static int now_y = 0;
   tft.setRotation(SCREEN_ROTATION);

   for (int y = 0; y < 24 * _SCALE; y++)
   {
      for (int x = 0; x < 32 * _SCALE; x++)
      {
         value = bio_linear_interpolation(x, y, mlx90640To_buffer);
         getColour(value);
         lineBuffer[x + now_y * 32 * _SCALE] = tft.color565(R_colour, G_colour, B_colour);
      }
      now_y++;
      if (now_y == lines)
      {
         if (dmaBufferSel)
            dmaBufferPtr = dmaBuffer2;
         else
            dmaBufferPtr = dmaBuffer1;
         dmaBufferSel = !dmaBufferSel; // Toggle buffer selection
         tft.startWrite();
         tft.pushImageDMA(0, y - now_y, 32 * _SCALE, lines, lineBuffer, dmaBufferPtr);
         tft.endWrite();
         now_y = 0;
      }
   }

   if (now_y != 0)
   {
      if (dmaBufferSel)
         dmaBufferPtr = dmaBuffer2;
      else
         dmaBufferPtr = dmaBuffer1;
      dmaBufferSel = !dmaBufferSel; // Toggle buffer selection
      tft.startWrite();
      tft.pushImageDMA(0, 24 * _SCALE - 1 - now_y, 32 * _SCALE, now_y, lineBuffer, dmaBufferPtr);
      tft.endWrite();
      now_y = 0;
   }

   // 热点追踪
   draw_cross(max_y * _SCALE, (24 - max_x) * _SCALE, 10, TFT_RED, 2);
   draw_cross(min_y * _SCALE, (24 - min_x) * _SCALE, 10, TFT_SKYBLUE, 2);
}

// 热成像读取多任务
void task_mlx(void *ptr)
{
   Wire.begin(MLX_SDA, MLX_SCL);
   pinMode(MLX_VDD, OUTPUT);
   digitalWrite(MLX_VDD, LOW);
   vTaskDelay(1000);
   Wire.setClock(800000); // Increase I2C clock speed to 400kHz

   Serial.println("MLX90640 IR Array Example");

   if (isConnected() == false)
   {
      while (1)
      {
         Serial.println("MLX90640 not detected at default I2C address. Please check wiring. Freezing.");
         vTaskDelay(1000);
      };
   }
   Serial.println("MLX90640 online!");
   int status;
   uint16_t eeMLX90640[832];

   status = MLX90640_DumpEE(MLX90640_address, eeMLX90640);
   if (status != 0)
      Serial.println("Failed to load system parameters");

   status = MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
   if (status != 0)
   {
      Serial.println("Parameter extraction failed");
      Serial.print(" status = ");
      Serial.println(status);
   }
   // Once params are extracted, we can release eeMLX90640 array
   MLX90640_I2CWrite(0x33, 0x800D, 6401); // writes the value 1901 (HEX) = 6401 (DEC) in the register at position 0x800D to enable reading out the temperatures!!!
   // ===============================================================================================================================================================
   // MLX90640_SetRefreshRate(MLX90640_address, 0x00); //Set rate to 0.25Hz effective - Works
   // MLX90640_SetRefreshRate(MLX90640_address, 0x01); //Set rate to 0.5Hz effective - Works
   // MLX90640_SetRefreshRate(MLX90640_address, 0x02); //Set rate to 1Hz effective - Works
   // MLX90640_SetRefreshRate(MLX90640_address, 0x03); //Set rate to 2Hz effective - Works
   // MLX90640_SetRefreshRate(MLX90640_address, 0x04); //Set rate to 4Hz effective - Works
   MLX90640_SetRefreshRate(MLX90640_address, 0x05); // Set rate to 8Hz effective - Works at 800kHz
   // MLX90640_SetRefreshRate(MLX90640_address, 0x06); //Set rate to 16Hz effective - fails
   // MLX90640_SetRefreshRate(MLX90640_address, 0x07); //Set rate to 32Hz effective - fails

   // MLX主循环
   for (; power_on == true;)
   {
      if (!freeze)
      { // 如果画面被暂停会跳过这个热成像图的刷新
         lock = true;
         for (byte x = 0; x < 2; x++)
         {
            uint16_t mlx90640Frame[834];
            int status = MLX90640_GetFrameData(MLX90640_address, mlx90640Frame);

            if (status < 0)
            {
               Serial.print("GetFrame Error: ");
               Serial.println(status);
            }
            float vdd = MLX90640_GetVdd(mlx90640Frame, &mlx90640);
            float Ta = MLX90640_GetTa(mlx90640Frame, &mlx90640);

            float tr = Ta - TA_SHIFT; // Reflected temperature based on the sensor ambient temperature
            float emissivity = 0.95;

            MLX90640_CalculateTo(mlx90640Frame, &mlx90640, emissivity, tr, mlx90640To);
         }
         // determine T_min and T_max and eliminate error pixels
         // ====================================================

         // mlx90640To[1*32 + 21] = 0.5 * (mlx90640To[1*32 + 20] + mlx90640To[1*32 + 22]);    // eliminate the error-pixels
         // mlx90640To[4*32 + 30] = 0.5 * (mlx90640To[4*32 + 29] + mlx90640To[4*32 + 31]);    // eliminate the error-pixels

         T_min = mlx90640To[0];
         T_max = mlx90640To[0];
         T_avg = mlx90640To[0];
         for (int i = 1; i < 768; i++)
         {
            if ((mlx90640To[i] > -41) && (mlx90640To[i] < 301))
            {
               if (mlx90640To[i] < T_min)
               {
                  T_min = mlx90640To[i];

                  min_x = i / 32;
                  min_y = i % 32;
               }

               if (mlx90640To[i] > T_max)
               {
                  T_max = mlx90640To[i];
                  max_x = i / 32;
                  max_y = i % 32;
               }
               mlx90640To[i] = KalmanFilter(&kfpVar3Array[i], mlx90640To[i]);
            }
            else if (i > 0) // temperature out of range
            {
               mlx90640To[i] = mlx90640To[i - 1];
            }
            else
            {
               mlx90640To[i] = mlx90640To[i + 1];
            }
            T_avg = T_avg + mlx90640To[i];
         }
         T_avg = T_avg / 768;
      }
      lock = false;
      vTaskDelay(5);
   }
   vTaskDelete(NULL);
}

// 关机
void power_off()
{

   esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
   for (int i = brightness; i > 0; i--)
   {
      ledcWrite(0, i);
      vTaskDelay(2);
   }

   power_on = false;
   digitalWrite(MLX_VDD, LOW);
   ledcWrite(0, 0);
   digitalWrite(SCREEN_BL_PIN, LOW);
   vTaskDelay(2000);
   // esp_light_sleep_start();
   esp_deep_sleep_start();
   vTaskDelete(NULL);
}

// 背光调节,会限制输入亮度在正确范围内
void set_brightness(int _brightness)
{
   if (_brightness < 1024 && _brightness > 5)
   {
      ledcWrite(0, _brightness);
      brightness = _brightness;
   }
   else if (_brightness >= 1024)
   {
      ledcWrite(0, 1023);
      brightness = 1023;
   }
   else if (_brightness <= 5)
   {
      ledcWrite(0, 5);
      brightness = 5;
   }
}

// 平滑的开机
void task_smooth_on(void *ptr)
{
   ledcSetup(0, 3000, 10);
   ledcAttachPin(SCREEN_BL_PIN, 0);
   ledcWrite(0, 0);
   vTaskDelay(500);
   for (int i = 0; i < brightness; i++)
   {
      ledcWrite(0, i);
      vTaskDelay(2);
   }
   vTaskDelete(NULL);
}

// 电池管理多任务
void task_bat(void *ptr)
{
   pinMode(BAT_ADC, INPUT);
   float r1 = 300.;
   float r2 = 680.;
   float coef = (r1 + r2) / r2;
   int adc_value = analogRead(BAT_ADC);
   for (; power_on == true;)
   {
      adc_value = analogRead(BAT_ADC);
      bat_v = (float)adc_value / 4096 * 3.3 * coef;
      vTaskDelay(1000);
   }
   vTaskDelete(NULL);
}

void task_button(void *ptr)
{
   pinMode(buttonPin1, INPUT_PULLUP);
   pinMode(buttonPin2, INPUT_PULLUP);
   unsigned long btn1_pushed_start_time = 0;
   unsigned long btn2_pushed_start_time = 0;
   bool btn2_pushed = false;
   bool btn1_pushed = false;
   for (; power_on == true;)
   {

      if (digitalRead(buttonPin1) == LOW)
      { // 长按btn1的关机功能
         if (millis() - btn1_pushed_start_time >= BTN_LONG_PUSH_T)
         {
            power_off();
         }
         vTaskDelay(5);
         if (digitalRead(buttonPin1) == LOW)
         {
            btn1_pushed = true;
         }
      }
      else
      {
         btn1_pushed_start_time = millis();
         if (btn1_pushed)
         { // 短按btn1
            test_points[0][0] = 0;
            test_points[0][1] = 0;
            if (freeze == true)
            {
               clear_local_temp = true;
            }
         }
         btn1_pushed = false;
      }

      if (digitalRead(buttonPin2) == LOW)
      { // 长按btn2的功能
         if (millis() - btn2_pushed_start_time >= BTN_LONG_PUSH_T)
         {
            power_off();
         }
         vTaskDelay(5);
         if (digitalRead(buttonPin2) == LOW)
         {
            btn2_pushed = true;
         }
      }
      else
      {
         btn2_pushed_start_time = millis();
         if (btn2_pushed)
         {
            freeze = !freeze;
         }
         btn2_pushed = false;
      }

      buttonState1 = digitalRead(buttonPin1);
      buttonState2 = digitalRead(buttonPin2);
      vTaskDelay(100);
   }
   vTaskDelete(NULL);
}

void task_touchpad(void *ptr)
{
   // touch.begin();
   uint16_t x, y;
   uint16_t start_x, start_y;
   bool long_pushed = false;
   unsigned long touch_pushed_start_time = 0;
   bool touched = false;
   int start_br = brightness;

   for (; power_on == true;)
   {
      while (touch_lock == true)
      {
         vTaskDelay(1);
      }
      touch.update();
      if (touch.tp.touching)
      {
         x = touch.tp.y;
         y = 240 - touch.tp.x;
         if (touched == false)
         {
            start_x = x;
            start_y = y;
            diffy = 0;
            diffx = 0;
         } // 下降沿
         if (millis() - touch_pushed_start_time >= TOUCH_LONG_PUSH_T)
         {
            long_pushed = true;
            diffx = start_x - x;
            diffy = start_y - y;
            set_brightness(start_br + diffy * 5);
         }
         else
         { // 短按的中间
         }
      }
      else
      {
         touch_pushed_start_time = millis();
         if (touched == true)
         { // 上升沿
            if (start_br == brightness)
            {
               if (y < 216)
               {
                  test_points[0][0] = x;
                  test_points[0][1] = y;
               }
            }
            if (long_pushed == false)
            { // 短按时
               if (y < 216)
               {
                  test_points[0][0] = x;
                  test_points[0][1] = y;
               }
            }
            start_br = brightness;
            long_pushed = false; // 上升沿将长按检测标识符进行复位
         }
      }
      // Serial.printf("touch: %d %d\n", touched, touch.tp.touching);
      touched = touch.tp.touching;
      vTaskDelay(10);
   }
   vTaskDelete(NULL);
}

void task_screen_draw(void *ptr)
{
   tft.setRotation(SCREEN_ROTATION);
   tft.fillScreen(TFT_BLACK);
   test_points[0][0] = 120;
   test_points[0][1] = 110;
   uint32_t dt = millis();
   // tft.setBitmapColor(16);
   for (; power_on == true;)
   {
      // 这么做是预防touch和spi总线发生冲突
      // if (!touch_lock){touch.update(); touch_lock=true;}

      if (!freeze)
      { // 如果画面被暂停会跳过这个热成像图的刷新
         // 只有画面更新才会绘制一张热成像图
         dt = millis();
         while (lock && power_on)
         {
            // 阻塞画面
            vTaskDelay(1);
         }
         for (int i = 0; i < 768; i++)
         {
            // mlx90640To_buffer[i] = mlx90640To[i];
            mlx90640To_buffer[i] = (int)(180.0 * (mlx90640To[i] - T_min) / (T_max - T_min));
         } // 拷贝温度信息
         // memcpy(mlx90640To_buffer, mlx90640To, 768 * sizeof(float));
         touch_lock = true;
         // draw_heat_image();
         draw_heat_image_dma();
         touch_lock = false;
         dt = millis() - dt;
      }
      else
      {
         dt = 0;
      }

      tft.setRotation(SCREEN_ROTATION);
      if (test_points[0][0] == 0 && test_points[0][1] == 0)
      {
      }
      else
      {
         show_local_temp(test_points[0][0], test_points[0][1]);
      }
      // if (show_local_temp_flag==true) {show_local_temp(test_points[0][0], test_points[0][1]);}
      if (clear_local_temp == true)
      {
         draw_heat_image_dma();
         clear_local_temp = false;
      }
      // tft.endWrite();

      tft.setRotation(SCREEN_ROTATION);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(25, 220);
      tft.printf("MAX: %.2f  ", T_max);
      tft.setCursor(25, 230);
      tft.printf("MIN: %.2f  ", T_min);

      tft.setCursor(105, 220);
      tft.printf("AVG: %.2f  ", T_avg);
      tft.setCursor(105, 230);
      tft.printf("BAT: %.2f   ", bat_v);

      tft.setCursor(180, 220);
      tft.printf("BRIGHT: %d  ", brightness);
      tft.setCursor(180, 230);
      tft.printf("TIME: %d ", dt);
      tft.printf("ms     ");

      vTaskDelay(10);
   }
   vTaskDelete(NULL);
}

#if defined(SEND_TO_SERIAL)
// 通过串口传输单个浮点数据
void send_float_as_uint8(float f, uint8_t *buf)
{
   memcpy(buf, &f, sizeof(float));
   Serial.write(buf, sizeof(float));
}

// 通过串口把整个温度数据矩阵传输
void send_to_serial()
{
   // memcpy(mlx90640To_Serial_buffer, mlx90640To_send_buffer, 768 * sizeof(float));
   Serial.write(mlx90640To_Serial_buffer, 768 * sizeof(float));
}

void task_serial_communicate(void *ptr)
{
   vTaskDelay(3000);
   uint8_t send_buf[4];

   for (; power_on == true;)
   {
      // 拷贝温度信息
      while (lock == true)
      {
         vTaskDelay(1);
      }
      memcpy(mlx90640To_send_buffer, mlx90640To, 768 * sizeof(float));
      // for (int i = 0; i < 768; i++) {mlx90640To_send_buffer[i] = mlx90640To[i];}
      Serial.print("BEGIN");
      send_float_as_uint8(T_max, send_buf);
      send_float_as_uint8(T_min, send_buf);
      send_float_as_uint8(T_avg, send_buf);
      // for (int i = 0; i < 768; i++){
      //    send_float_as_uint8(mlx90640To_send_buffer[i], send_buf);
      //    if(i % 5==0){vTaskDelay(1);}
      // }
      send_to_serial();
      Serial.print("END");
      vTaskDelay(10);
   }
   vTaskDelete(NULL);
}
#endif

void setup(void)
{

   Serial.begin(921600);

   pinMode(11, OUTPUT);

   touch.begin();
   // 按钮启用

   pinMode(SCREEN_BL_PIN, OUTPUT);
   vTaskDelay(500);
   xTaskCreate(task_mlx, "MLX_FLASHING", 1024 * 8, NULL, 1, NULL);
   xTaskCreate(task_bat, "BAT_MANAGER", 1024 * 2, NULL, 3, NULL);
   tft.init();
   tft.setSwapBytes(true);
   tft.initDMA();
   xTaskCreatePinnedToCore(task_screen_draw, "SCREEN", 1024 * 15, NULL, 2, NULL, 0);
   xTaskCreate(task_smooth_on, "SMOOTH_ON", 1024, NULL, 2, NULL);
   xTaskCreate(task_button, "BUTTON", 1024 * 8, NULL, 3, NULL);
   xTaskCreatePinnedToCore(task_touchpad, "TOUCHPAD", 1024 * 3, NULL, 3, NULL, 1);
#if defined(SEND_TO_SERIAL)
   xTaskCreate(task_serial_communicate, "SERIAL_COMM", 1024 * 10, NULL, 5, NULL);
#endif
}

void loop()
{
   //  if (!touch_updated){touch.update(); touch_updated=true;}
   vTaskDelay(3000);
}
