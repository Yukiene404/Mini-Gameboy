/*
  KEYCHAIN BOY - TFT edition
  Board  : ESP32 (classic, e.g. ESP32 Dev Module)
  Screen : 2.4" 240x320 8-bit parallel TFT (assumed ILI9341) -> LovyanGFX
  Buttons: LEFT GPIO4, START GPIO5 (= A), RIGHT GPIO13  (other side of each button to GND)

  MENU: hold LEFT + RIGHT together for ~0.4 s to go back to the menu.

  Library: install "LovyanGFX" from the Arduino Library Manager.
*/

#include <LovyanGFX.hpp>
#include <Preferences.h>
// ------------------------------------------------------------------ hardware options
// 1 and 3 are the two landscape orientations.  This board is mounted 180°
// from the original sketch, so 3 is the appropriate default.
constexpr uint8_t TFT_ROTATION = 3;

// Set true ONLY when black/white (or all colours) are photographic negatives.
// This does not rotate the display.
constexpr bool TFT_INVERT_COLOURS = false;

// GPIO16 is the unused, likely backlight line on this ESP32 wiring.  Confirm
// that the TFT's LED/BL pad is actually connected to GPIO16.  Use -1 when the
// module's LED pin is wired directly to its supply instead.
constexpr int TFT_BACKLIGHT_PIN = 16;
constexpr bool TFT_BACKLIGHT_INVERTED = true;
constexpr uint8_t TFT_BRIGHTNESS = 255; // 0..255; 255 is maximum

// The stated wiring is button -> GND, which requires active-low pull-ups.
// Change to false only if every button instead connects its GPIO to 3V3.
constexpr bool BUTTONS_ACTIVE_LOW = true;
constexpr uint16_t BUTTON_DEBOUNCE_MS = 20;

// ------------------------------------------------------------------ display setup
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr = 21;
      cfg.pin_rd = -1;
      cfg.pin_rs = 33;
      cfg.pin_d0 = 23; cfg.pin_d1 = 22; cfg.pin_d2 = 25; cfg.pin_d3 = 26;
      cfg.pin_d4 = 27; cfg.pin_d5 = 14; cfg.pin_d6 = 18; cfg.pin_d7 = 19;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs  = 32;
      cfg.pin_rst = 17;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = false;
      cfg.invert = TFT_INVERT_COLOURS;
      cfg.rgb_order = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    if (TFT_BACKLIGHT_PIN >= 0) {
      auto cfg = _light.config();
      cfg.pin_bl = TFT_BACKLIGHT_PIN;
      cfg.invert = TFT_BACKLIGHT_INVERTED;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
LGFX tft;
LGFX_Sprite display(&tft);        // 128x64 virtual screen, scaled up onto the TFT

#define SCREEN_W 128
#define SCREEN_H 64
#define FRAME_MS 33               // ~30 fps
#define SCALE    2.5f             // 128*2.5 = 320 (fills landscape width). Use 2.0f for a faster, crisper integer scale.

uint16_t fg = 0xFFFF;             // current foreground colour (changes per game)
#define COL_ON  fg
#define COL_OFF ((uint16_t)0x0000)

// RGB565 colours
const uint16_t C_WHITE = 0xFFFF, C_GREEN = 0x07E0, C_CYAN = 0x07FF,
               C_YELLOW = 0xFFE0, C_ORANGE = 0xFD20, C_PINK = 0xF81F;

Preferences prefs;

// ------------------------------------------------------------------ buttons
enum { BTN_L, BTN_R, BTN_A, BTN_MENU };
// {LEFT, RIGHT, START(A), virtual MENU}
const uint8_t BTN_PIN[3] = {4, 13, 5};
bool held[4] = {};
bool prevHeld[4] = {};
bool rawHeld[3] = {};
uint32_t rawChangedAt[3] = {};

bool readButtonPin(uint8_t pin) {
  int level = digitalRead(pin);
  return BUTTONS_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

void readButtons() {
  static uint32_t comboAt = 0;
  static bool comboFired = false;
  const uint32_t now = millis();

  for (int i = 0; i < 4; i++) prevHeld[i] = held[i];

  for (int i = 0; i < 3; i++) {
    bool raw = readButtonPin(BTN_PIN[i]);
    if (raw != rawHeld[i]) {
      rawHeld[i] = raw;
      rawChangedAt[i] = now;
    }
    if (now - rawChangedAt[i] >= BUTTON_DEBOUNCE_MS) {
      held[i] = rawHeld[i];
    }
  }

  // Virtual MENU button: LEFT+RIGHT held together for 0.4 s.
  held[BTN_MENU] = false;
  if (held[BTN_L] && held[BTN_R]) {
    if (comboAt == 0) comboAt = now;
    else if (!comboFired && now - comboAt >= 400) {
      held[BTN_MENU] = true;     // true for one frame
      comboFired = true;
    }
  } else {
    comboAt = 0;
    comboFired = false;
  }
}

inline bool pressed(int i) { return held[i] && !prevHeld[i]; }
// ------------------------------------------------------------------ shared state
enum { MENU = -1, INVADERS = 0, DINO = 1, FLAPPY = 2, SNAKE = 3, BREAKOUT = 4 };
const int NUM_GAMES = 5;

const char *GAME_NAMES[NUM_GAMES] = {"Alien Attack", "T-Rex Dash", "Flap Jump", "Snake", "Brick Buster"};
const uint16_t GAME_COLORS[NUM_GAMES] = {C_GREEN, C_ORANGE, C_YELLOW, C_CYAN, C_PINK};

int current = MENU;
int menuSel = 0;
uint16_t hiScore[NUM_GAMES];
int score = 0;
bool gameOver = false, newHigh = false;
uint32_t overAt = 0;

void loadScores() {
  prefs.begin("kboy", false);
  for (int i = 0; i < NUM_GAMES; i++) {
    char k[4];
    snprintf(k, sizeof(k), "h%d", i);
    hiScore[i] = prefs.getUShort(k, 0);
  }
}

void saveScore(int g) {
  char k[4];
  snprintf(k, sizeof(k), "h%d", g);
  prefs.putUShort(k, hiScore[g]);
}

void resetCommon() {
  score = 0;
  gameOver = false;
  newHigh = false;
}

void triggerGameOver() {
  if (gameOver) return;
  gameOver = true;
  overAt = millis();
  newHigh = false;
  if (score > hiScore[current]) {
    hiScore[current] = score;
    newHigh = true;
    saveScore(current);
  }
}

// returns true while the game-over screen is up (game logic should stop)
bool handleOver(void (*restart)()) {
  if (!gameOver) return false;
  if (millis() - overAt > 600) {          // ignore mashing right after dying
    if (pressed(BTN_A)) {
      restart();
    } else if (pressed(BTN_MENU) || pressed(BTN_L) || pressed(BTN_R)) {
      resetCommon();
      current = MENU;
    }
  }
  return true;
}

void drawGameOver() {
  display.fillRect(16, 12, 96, 42, COL_OFF);
  display.drawRect(16, 12, 96, 42, COL_ON);

  display.setTextSize(1);
  display.setTextColor(COL_ON);

  display.setCursor(38, 16);
  display.print("GAME OVER");

  display.setCursor(24, 28);
  display.print("Score: ");
  display.print(score);

  display.setCursor(24, 38);
  if (newHigh) {
    display.print("NEW BEST!");
  } else {
    display.print("Best:  ");
    display.print(hiScore[current]);
  }
}

// ================================================================== ALIEN ATTACK (INVADERS)
const uint8_t SPR_ALIEN0[] PROGMEM = {0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24, 0x5A, 0xA5};
const uint8_t SPR_ALIEN1[] PROGMEM = {0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x5A, 0x81, 0x42};
const uint8_t SPR_SHIP[]   PROGMEM = {0x18, 0x3C, 0x3C, 0x7E, 0xFF, 0xFF};

#define INV_COLS 6
#define INV_ROWS 3
bool invAlive[INV_ROWS][INV_COLS];
int invX, invY, invDir, invCount, invWave, invTick, invFrame;
int shipX, lives;
bool pbActive;
int pbX, pbY;
struct EBullet { bool on; int x, y; };
EBullet ebul[3];

void invNewWave() {
  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++) invAlive[r][c] = true;
  invCount = INV_ROWS * INV_COLS;
  invX = 8;
  invY = 10 + min(invWave, 3) * 4;
  invDir = 1;
  invTick = 0;
  invFrame = 0;
  pbActive = false;
  for (auto &b : ebul) b.on = false;
}

void initInvaders() {
  resetCommon();
  lives = 3;
  invWave = 0;
  shipX = 60;
  invNewWave();
}

void updateInvaders() {
  if (handleOver(initInvaders)) return;

  if (held[BTN_L]) shipX -= 3;
  if (held[BTN_R]) shipX += 3;
  shipX = constrain(shipX, 0, 120);

  if (held[BTN_A] && !pbActive) { pbActive = true; pbX = shipX + 4; pbY = 52; }

  if (pbActive) {
    pbY -= 4;
    if (pbY < 8) pbActive = false;
    else {
      bool hit = false;
      for (int r = 0; r < INV_ROWS && !hit; r++)
        for (int c = 0; c < INV_COLS && !hit; c++) {
          if (!invAlive[r][c]) continue;
          int ax = invX + c * 12, ay = invY + r * 10;
          if (pbX >= ax && pbX < ax + 8 && pbY >= ay && pbY < ay + 8) {
            invAlive[r][c] = false;
            invCount--;
            score += 10 * (INV_ROWS - r);
            pbActive = false;
            hit = true;
          }
        }
    }
  }

  if (invCount == 0) { invWave++; invNewWave(); return; }

  if (++invTick >= 2 + invCount) {
    invTick = 0;
    invFrame ^= 1;
    int minC = INV_COLS, maxC = -1, maxR = -1;
    for (int r = 0; r < INV_ROWS; r++)
      for (int c = 0; c < INV_COLS; c++)
        if (invAlive[r][c]) { minC = min(minC, c); maxC = max(maxC, c); maxR = max(maxR, r); }
    int nx = invX + invDir * 3;
    if (nx + minC * 12 < 0 || nx + maxC * 12 + 8 > SCREEN_W) { invDir = -invDir; invY += 4; }
    else invX = nx;
    if (invY + maxR * 10 + 8 >= 54) { triggerGameOver(); return; }
  }

  if (random(0, 22) == 0) {
    int c = random(0, INV_COLS), r = -1;
    for (int rr = INV_ROWS - 1; rr >= 0; rr--) if (invAlive[rr][c]) { r = rr; break; }
    if (r >= 0)
      for (auto &b : ebul) if (!b.on) { b.on = true; b.x = invX + c * 12 + 4; b.y = invY + r * 10 + 8; break; }
  }
  for (auto &b : ebul) {
    if (!b.on) continue;
    b.y += 2;
    if (b.y >= 64) b.on = false;
    else if (b.y >= 56 && b.y < 62 && b.x >= shipX && b.x < shipX + 8) {
      b.on = false;
      if (--lives <= 0) { triggerGameOver(); return; }
    }
  }
}

void drawInvaders() {
  display.setTextSize(1);
  display.setTextColor(COL_ON);
  display.setCursor(0, 0);
  display.print("S:"); display.print(score);
  display.setCursor(92, 0);
  display.print("HP:"); display.print(lives);

  for (int r = 0; r < INV_ROWS; r++)
    for (int c = 0; c < INV_COLS; c++)
      if (invAlive[r][c])
        display.drawBitmap(invX + c * 12, invY + r * 10, invFrame ? SPR_ALIEN1 : SPR_ALIEN0, 8, 8, COL_ON);

  display.drawBitmap(shipX, 56, SPR_SHIP, 8, 6, COL_ON);
  if (pbActive) display.drawFastVLine(pbX, pbY, 3, COL_ON);
  for (auto &b : ebul) if (b.on) display.drawFastVLine(b.x, b.y, 3, COL_ON);
  display.drawFastHLine(0, 63, SCREEN_W, COL_ON);
  if (gameOver) drawGameOver();
}

// ================================================================== T-REX DASH (DINO)
#define GROUND 56
#define DINO_X 14
struct Obs { bool on; uint8_t type; float x; int w, h, y; };
Obs obs[3];
float dinoH, dinoV, dinoSpeed, dinoScroll;
bool dinoDuck;
int dinoTick, dinoGap;

void initDino() {
  resetCommon();
  dinoH = 0; dinoV = 0; dinoSpeed = 3.0f; dinoScroll = 0;
  dinoDuck = false; dinoTick = 0; dinoGap = 40;
  for (auto &o : obs) o.on = false;
}

void updateDino() {
  if (handleOver(initDino)) return;

  dinoTick++;
  if (dinoTick % 3 == 0) score++;

  dinoSpeed = min(6.0f, 3.0f + score / 150.0f);
  dinoScroll += dinoSpeed;

  bool downKey = held[BTN_L] || held[BTN_R];
  dinoDuck = downKey && dinoH <= 0;

  if (dinoH <= 0 && pressed(BTN_A)) dinoV = 5.6f;

  if (dinoH > 0 || dinoV > 0) {
    dinoH += dinoV;
    dinoV -= 0.6f;
    if (downKey) dinoV -= 0.7f;
    if (dinoH <= 0) { dinoH = 0; dinoV = 0; }
  }

  float rightmost = -1000;
  for (auto &o : obs) {
    if (!o.on) continue;
    o.x -= dinoSpeed;
    if (o.x + o.w < 0) o.on = false;
    else if (o.x + o.w > rightmost) rightmost = o.x + o.w;
  }
  if (rightmost < SCREEN_W - dinoGap) {
    for (auto &o : obs) {
      if (o.on) continue;
      o.on = true;
      o.x = SCREEN_W;
      if (score > 200 && random(0, 4) == 0) { o.type = 1; o.w = 10; o.h = 7; o.y = 40; }
      else { o.type = 0; o.w = (random(0, 3) == 0) ? 13 : 7; o.h = random(10, 17); o.y = GROUND - o.h; }
      dinoGap = (int)(dinoSpeed * 20) + 20 + random(0, 50);
      break;
    }
  }

  int fy = GROUND - (int)dinoH;
  int hx, hw, top, hh;
  if (dinoDuck) { hx = DINO_X - 1; hw = 13; top = fy - 6;  hh = 6; }
  else          { hx = DINO_X + 1; hw = 10; top = fy - 13; hh = 13; }
  for (auto &o : obs) {
    if (!o.on) continue;
    int ox = (int)o.x;
    if (hx < ox + o.w && hx + hw > ox && top < o.y + o.h && top + hh > o.y) { triggerGameOver(); return; }
  }
}

void drawDino() {
  display.setTextSize(1);
  display.setTextColor(COL_ON);
  display.setCursor(48, 0);
  display.print("HI "); display.print(hiScore[DINO]);
  display.print("  "); display.print(score);

  display.drawFastHLine(0, GROUND, SCREEN_W, COL_ON);
  for (int i = 0; i < 10; i++) {
    int px = ((i * 37) - (int)dinoScroll) % SCREEN_W;
    if (px < 0) px += SCREEN_W;
    display.drawPixel(px, 58 + (i % 3) * 2, COL_ON);
  }

  int fy = GROUND - (int)dinoH;
  bool legA = (dinoTick / 4) % 2;
  bool air = dinoH > 0;
  if (!dinoDuck) {
    display.fillRect(DINO_X - 3, fy - 8, 3, 2, COL_ON);
    display.fillRect(DINO_X, fy - 9, 9, 6, COL_ON);
    display.fillRect(DINO_X + 4, fy - 14, 8, 6, COL_ON);
    display.drawPixel(DINO_X + 9, fy - 13, COL_OFF);
    display.fillRect(DINO_X + 1, fy - 3, 2, (air || legA) ? 3 : 2, COL_ON);
    display.fillRect(DINO_X + 6, fy - 3, 2, (air || !legA) ? 3 : 2, COL_ON);
  } else {
    display.fillRect(DINO_X - 3, fy - 7, 17, 5, COL_ON);
    display.drawPixel(DINO_X + 11, fy - 6, COL_OFF);
    display.fillRect(DINO_X + 1, fy - 2, 2, legA ? 2 : 1, COL_ON);
    display.fillRect(DINO_X + 8, fy - 2, 2, legA ? 1 : 2, COL_ON);
  }

  for (auto &o : obs) {
    if (!o.on) continue;
    int ox = (int)o.x, oy = o.y;
    if (o.type == 0) {
      for (int ux = ox; ux < ox + o.w; ux += 6) {
        display.fillRect(ux + 2, oy, 3, o.h, COL_ON);
        display.fillRect(ux, oy + 3, 2, 4, COL_ON);
        display.fillRect(ux, oy + 6, 3, 1, COL_ON);
        display.fillRect(ux + 5, oy + 4, 2, 4, COL_ON);
        display.fillRect(ux + 4, oy + 7, 3, 1, COL_ON);
      }
    } else {
      display.fillRect(ox + 2, oy + 2, 7, 3, COL_ON);
      display.fillRect(ox, oy + 2, 2, 2, COL_ON);
      if ((dinoTick / 5) % 2) display.fillRect(ox + 4, oy, 3, 2, COL_ON);
      else                    display.fillRect(ox + 4, oy + 5, 3, 2, COL_ON);
    }
  }
  if (gameOver) drawGameOver();
}

// ================================================================== FLAP JUMP (FLAPPY)
#define PIPE_W 12
#define PIPE_GAP 26
#define PIPE_SPACING 56
#define BIRD_X 28
struct Pipe { float x; int gapY; bool passed; };
Pipe pipes[3];
float birdY, birdV;
bool flapStarted;
int flapTick;

void newPipe(int i, float x) {
  pipes[i].x = x;
  pipes[i].gapY = random(10, 64 - 10 - PIPE_GAP + 1);
  pipes[i].passed = false;
}

void initFlappy() {
  resetCommon();
  birdY = 28; birdV = 0; flapStarted = false; flapTick = 0;
  for (int i = 0; i < 3; i++) newPipe(i, 150 + i * PIPE_SPACING);
}

void updateFlappy() {
  if (handleOver(initFlappy)) return;
  bool flap = pressed(BTN_A) || pressed(BTN_L) || pressed(BTN_R);
  flapTick++;

  if (!flapStarted) {
    birdY = 28 + sinf(flapTick * 0.2f) * 3;
    if (flap) { flapStarted = true; birdV = -3.0f; }
    return;
  }

  if (flap) birdV = -3.0f;
  birdV += 0.32f;
  if (birdV > 4) birdV = 4;
  birdY += birdV;
  if (birdY < 3) { birdY = 3; birdV = 0; }
  if (birdY >= 60) { triggerGameOver(); return; }

  for (auto &p : pipes) p.x -= 1.6f;
  for (int i = 0; i < 3; i++) {
    if (pipes[i].x < -PIPE_W) {
      float mx = 0;
      for (auto &q : pipes) if (q.x > mx) mx = q.x;
      newPipe(i, mx + PIPE_SPACING);
    }
  }
  for (auto &p : pipes) {
    if (!p.passed && p.x + PIPE_W < BIRD_X - 3) { p.passed = true; score++; }
    if (BIRD_X + 3 > p.x && BIRD_X - 3 < p.x + PIPE_W) {
      if (birdY - 3 < p.gapY || birdY + 3 > p.gapY + PIPE_GAP) { triggerGameOver(); return; }
    }
  }
}

void drawFlappy() {
  for (auto &p : pipes) {
    int px = (int)p.x;
    display.drawRect(px, -1, PIPE_W, p.gapY + 1, COL_ON);
    display.drawRect(px - 1, p.gapY - 4, PIPE_W + 2, 4, COL_ON);
    display.drawRect(px, p.gapY + PIPE_GAP, PIPE_W, 64 - (p.gapY + PIPE_GAP) + 1, COL_ON);
    display.drawRect(px - 1, p.gapY + PIPE_GAP, PIPE_W + 2, 4, COL_ON);
  }
  display.drawFastHLine(0, 63, SCREEN_W, COL_ON);

  int by = (int)birdY;
  display.fillCircle(BIRD_X, by, 3, COL_ON);
  display.drawPixel(BIRD_X + 1, by - 1, COL_OFF);
  display.fillRect(BIRD_X + 3, by, 2, 1, COL_ON);

  display.fillRect(0, 0, 30, 9, COL_OFF);
  display.setTextSize(1);
  display.setTextColor(COL_ON);
  display.setCursor(0, 0);
  display.print(score);

  if (!flapStarted) { display.setCursor(25, 46); display.print("Press to flap"); }
  if (gameOver) drawGameOver();
}

// ================================================================== SNAKE
#define SN_W 30
#define SN_H 13
#define SN_ORG_X 4
#define SN_ORG_Y 10
#define SN_MAX (SN_W * SN_H)
uint8_t snX[SN_MAX], snY[SN_MAX];
int snLen, snDir, snTurn, snTick, foodX, foodY;
const int8_t SN_DX[4] = {0, 1, 0, -1};
const int8_t SN_DY[4] = {-1, 0, 1, 0};

void snPlaceFood() {
  while (true) {
    foodX = random(0, SN_W);
    foodY = random(0, SN_H);
    bool ok = true;
    for (int i = 0; i < snLen; i++) if (snX[i] == foodX && snY[i] == foodY) { ok = false; break; }
    if (ok) return;
  }
}

void initSnake() {
  resetCommon();
  snLen = 4; snDir = 1; snTurn = 0; snTick = 0;
  for (int i = 0; i < snLen; i++) { snX[i] = 15 - i; snY[i] = 6; }
  snPlaceFood();
}

void updateSnake() {
  if (handleOver(initSnake)) return;
  if (pressed(BTN_L)) snTurn = -1;
  if (pressed(BTN_R)) snTurn = 1;

  int interval = max(3, 7 - (snLen - 4) / 5);
  if (++snTick < interval) return;
  snTick = 0;

  if (snTurn) { snDir = (snDir + 4 + snTurn) % 4; snTurn = 0; }
  int nx = snX[0] + SN_DX[snDir], ny = snY[0] + SN_DY[snDir];
  if (nx < 0 || nx >= SN_W || ny < 0 || ny >= SN_H) { triggerGameOver(); return; }

  bool eat = (nx == foodX && ny == foodY);
  int chk = eat ? snLen : snLen - 1;
  for (int i = 0; i < chk; i++)
    if (snX[i] == nx && snY[i] == ny) { triggerGameOver(); return; }

  if (eat && snLen < SN_MAX) snLen++;
  memmove(&snX[1], &snX[0], snLen - 1);
  memmove(&snY[1], &snY[0], snLen - 1);
  snX[0] = nx; snY[0] = ny;
  if (eat) { score += 10; snPlaceFood(); }
}

void drawSnake() {
  display.setTextSize(1);
  display.setTextColor(COL_ON);
  display.setCursor(0, 0);
  display.print("S:"); display.print(score);
  display.setCursor(80, 0);
  display.print("HI:"); display.print(hiScore[SNAKE]);

  display.drawRect(2, 8, 124, 56, COL_ON);
  for (int i = 0; i < snLen; i++)
    display.fillRect(SN_ORG_X + snX[i] * 4, SN_ORG_Y + snY[i] * 4, 3, 3, COL_ON);
  display.drawRect(SN_ORG_X + foodX * 4, SN_ORG_Y + foodY * 4, 3, 3, COL_ON);
  if (gameOver) drawGameOver();
}

// ================================================================== BRICK BUSTER (BREAKOUT)
#define BR_COLS 8
#define BR_ROWS 4
#define PAD_W 22
bool brick[BR_ROWS][BR_COLS];
int brLeft, padX;
float bx, by, bvx, bvy, bspeed;
bool stuck;

void brickReset() {
  for (int r = 0; r < BR_ROWS; r++)
    for (int c = 0; c < BR_COLS; c++) brick[r][c] = true;
  brLeft = BR_ROWS * BR_COLS;
}

void initBreakout() {
  resetCommon();
  brickReset();
  lives = 3; bspeed = 2.0f; padX = (SCREEN_W - PAD_W) / 2; stuck = true;
}

int brickAt(float fx, float fy) {
  int x = (int)fx, y = (int)fy;
  if (y < 12 || x < 0 || x >= SCREEN_W) return -1;
  int r = (y - 12) / 6, c = x / 16;
  if (r >= BR_ROWS) return -1;
  if ((y - 12) % 6 >= 5 || x % 16 >= 15) return -1;
  return brick[r][c] ? r * BR_COLS + c : -1;
}

bool brickHit(int idx) {
  if (idx < 0) return false;
  int r = idx / BR_COLS, c = idx % BR_COLS;
  brick[r][c] = false;
  brLeft--;
  score += (BR_ROWS - r) * 5;
  if (bspeed < 3.2f) bspeed += 0.02f;
  return true;
}

void updateBreakout() {
  if (handleOver(initBreakout)) return;

  if (held[BTN_L]) padX -= 3;
  if (held[BTN_R]) padX += 3;
  padX = constrain(padX, 0, SCREEN_W - PAD_W);

  if (stuck) {
    bx = padX + PAD_W / 2; by = 58;
    if (pressed(BTN_A)) {
      stuck = false;
      bvx = random(0, 2) ? 1.0f : -1.0f;
      bvy = -sqrtf(bspeed * bspeed - bvx * bvx);
    }
    return;
  }

  bx += bvx;
  if (bx < 1) { bx = 1; bvx = -bvx; }
  if (bx > 126) { bx = 126; bvx = -bvx; }
  if (brickHit(brickAt(bx, by))) bvx = -bvx;

  by += bvy;
  if (by < 9) { by = 9; bvy = -bvy; }
  if (brickHit(brickAt(bx, by))) bvy = -bvy;

  if (brLeft == 0) { brickReset(); bspeed += 0.3f; stuck = true; return; }

  if (bvy > 0 && by >= 58 && by <= 62 && bx >= padX - 1 && bx <= padX + PAD_W + 1) {
    float rel = (bx - (padX + PAD_W / 2.0f)) / (PAD_W / 2.0f);
    rel = constrain(rel, -1.0f, 1.0f);
    bvx = rel * bspeed * 0.85f;
    bvy = -sqrtf(max(0.5f, bspeed * bspeed - bvx * bvx));
    by = 57;
  }

  if (by >= 64) {
    if (--lives <= 0) triggerGameOver();
    else stuck = true;
  }
}

void drawBreakout() {
  display.setTextSize(1);
  display.setTextColor(COL_ON);
  display.setCursor(0, 0);
  display.print("S:"); display.print(score);
  display.setCursor(92, 0);
  display.print("HP:"); display.print(lives);

  for (int r = 0; r < BR_ROWS; r++)
    for (int c = 0; c < BR_COLS; c++)
      if (brick[r][c]) display.fillRect(c * 16, 12 + r * 6, 15, 5, COL_ON);

  display.fillRect(padX, 60, PAD_W, 3, COL_ON);
  display.fillRect((int)bx - 1, (int)by - 1, 2, 2, COL_ON);
  if (stuck && !gameOver) { display.setCursor(40, 44); display.print("A: launch"); }
  if (gameOver) drawGameOver();
}

// ================================================================== MENU
void startGame(int g) {
  current = g;
  switch (g) {
    case INVADERS: initInvaders(); break;
    case DINO:     initDino();     break;
    case FLAPPY:   initFlappy();   break;
    case SNAKE:    initSnake();    break;
    case BREAKOUT: initBreakout(); break;
  }
}

void updateMenu() {
  if (pressed(BTN_L)) menuSel = (menuSel + NUM_GAMES - 1) % NUM_GAMES;
  if (pressed(BTN_R)) menuSel = (menuSel + 1) % NUM_GAMES;
  if (pressed(BTN_A)) startGame(menuSel);
}

void drawMenu() {
  display.setTextSize(1);
  display.setTextColor(COL_ON);

  display.setCursor(28, 1);
  display.print("KEYCHAIN BOY");
  display.drawFastHLine(0, 10, SCREEN_W, COL_ON);

  for (int i = 0; i < NUM_GAMES; i++) {
    int y = 14 + i * 10;
    if (i == menuSel) {
      display.setCursor(4, y);
      display.print(">");
    }
    display.setCursor(14, y);
    display.print(GAME_NAMES[i]);

    if (hiScore[i] > 0) {
      display.setCursor(104, y);
      display.print(hiScore[i]);
    }
  }
}

// ================================================================== MAIN
void setup() {
  for (int i = 0; i < 3; i++) {
    pinMode(BTN_PIN[i], BUTTONS_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  randomSeed(micros());
  loadScores();

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setBrightness(TFT_BRIGHTNESS);
  tft.fillScreen(0x0000);

  display.setColorDepth(16);
  display.createSprite(SCREEN_W, SCREEN_H);
  display.setFont(&fonts::Font0);
  display.setTextWrap(false);
}
void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < FRAME_MS) return;
  last = now;

  readButtons();

  // LEFT+RIGHT held -> back to menu
  if (current != MENU && pressed(BTN_MENU)) {
    resetCommon();
    current = MENU;
  }

  switch (current) {
    case MENU:     updateMenu();     break;
    case INVADERS: updateInvaders(); break;
    case DINO:     updateDino();     break;
    case FLAPPY:   updateFlappy();   break;
    case SNAKE:    updateSnake();    break;
    case BREAKOUT: updateBreakout(); break;
  }

  fg = (current == MENU) ? C_WHITE : GAME_COLORS[current];

  display.fillScreen(COL_OFF);

  switch (current) {
    case MENU:     drawMenu();     break;
    case INVADERS: drawInvaders(); break;
    case DINO:     drawDino();     break;
    case FLAPPY:   drawFlappy();   break;
    case SNAKE:    drawSnake();    break;
    case BREAKOUT: drawBreakout(); break;
  }

  // scale the 128x64 game screen up onto the centre of the 320x240 TFT
  display.pushRotateZoom(&tft, tft.width() / 2, tft.height() / 2, 0, SCALE, SCALE);
}