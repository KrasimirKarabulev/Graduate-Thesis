// ============================================================================
//  SMART LOCK - интелигентна брава на базата на ESP32
//
//  Отключване с RFID карта или с 4-цифрен код от клавиатурата.
//  Състоянието се показва на ILI9341 дисплей (320x240, landscape).
//  Картите и допълнителните кодове се пазят във флаш паметта (NVS),
//  така че оцеляват след рестарт или спиране на захранването.
//
//  Режими на работа:
//    - нормален : сканиране на карта или въвеждане на код + '#'
//    - админ    : добавяне/премахване на карти и кодове. Влизането е
//                 двустепенно - админ код + '#', след което в рамките на
//                 15 секунди трябва да се поднесе главната карта. В този
//                 прозорец се допускат до 3 грешни карти; прозорец с поне
//                 една грешна карта се брои за един неуспешен опит.
//    - заключен : след 3 грешни опита; освобождава се с валидна карта
//                 или автоматично след 60 секунди
// ============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <vector>
#include <Preferences.h>

// Библиотеки за дисплея
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// Библиотеки за следене на флаш паметта на ESP-то
#include <nvs.h>
#include <nvs_flash.h>

// --- КОНСТАНТИ ЗА ФЛАШ ПАМЕТТА (NVS) ---
#define NVS_ENTRY_SIZE 32          // всеки NVS запис заема 32 байта
#define NVS_NAMESPACE  "smart-lock" // името на нашето NVS пространство
size_t lastNsUsed = 0;             // предишният брой заети записи - за да смятаме разликата

// --- КОНФИГУРАЦИЯ НА КЛАВИАТУРАТА ---

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {36, 39, 34, 35};  // само за вход (input-only), с външни 10k pull-up резистори
byte colPins[COLS] = {19, 18, 21, 32};  // колона 4 (GPIO32) носи клавишите A, B, C, D

Keypad kpd = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- ПИНОВЕ НА ИЗПЪЛНИТЕЛНИТЕ ЕЛЕМЕНТИ ---
#define LOCK 12     // реле/електромагнит на бравата (HIGH = отключено)
#define BUZZER 13   // зумер
#define LED_OK 22   // зелен светодиод
#define LED_ERR 23  // червен светодиод

// --- ПИНОВЕ НА RFID ЧЕТЕЦА (MFRC522) ---
#define RST_PIN 33
#define SS_PIN 14
#define SCK_PIN 27
#define MISO_PIN 25
#define MOSI_PIN 26


// --- ПИНОВЕ НА ДИСПЛЕЯ ---
#define TFT_CS   5
#define TFT_DC   17
#define TFT_RST  16
// Подсветката е вързана директно към 3.3V - няма пин за управлението ѝ

// --- ПИН НА БУТОНА ЗА ИЗЛИЗАНЕ ОТВЪТРЕ (Request-To-Exit) ---
#define REX_BTN 4   // S2, активен при LOW, с вътрешен INPUT_PULLUP

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences preferences;

// --- ПАРОЛИ И ДАННИ ---
String masterPassword = "1234";    // Главна парола
String adminPassword = "9999";     // Парола за админ режим
String inputBuffer = "";           // Текущо въведените цифри от клавиатурата

// Главната карта е зашита в кода - не може да бъде изтрита дори при пълно нулиране
const byte MASTER_UID[]   = {0xA3, 0x48, 0x3B, 0xCD};
const byte MASTER_UID_LEN = 4;

// Един запис за карта: UID (до 10 байта) + реалната му дължина
struct CardUID {
    byte uid[10];
    byte size;
};
std::vector<CardUID> authorizedCards;    // Динамични карти (записват се в NVS)
std::vector<String> authorizedPasswords; // Динамични пароли (записват се в NVS)

// --- ЛОГИЧЕСКИ ПРОМЕНЛИВИ ---

// Ако не се натисне клавиш за INPUT_TIMEOUT ms, въведеното се изчиства.
// В нормален режим това се отчита и като отказан достъп.
unsigned long lastKeyPressTime = 0;
const unsigned long INPUT_TIMEOUT = 15000;

// Блокиране на клавиатурата след 3 поредни грешни кода
int failedAttempts = 0;
bool isKeypadLocked = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION = 60000;

bool isAdminMode = false;

// Двустепенно влизане в админ режим:
// самият админ код не дава права - той само отваря прозорец, в който
// трябва да се поднесе главната карта. Докато isAdminPending е true,
// системата още е в нормален режим и няма никакви админ правомощия.
bool isAdminPending = false;
unsigned long adminCardWaitStart = 0;               // начало на прозореца
const unsigned long ADMIN_CARD_TIMEOUT = 15000;     // 15 s за поднасяне на картата

// Грешни карти в рамките на текущия прозорец. Прозорецът не се затваря
// при първата грешка - остават общо 3 опита, преди да бъде прекратен.
// Броячът се нулира при всяко ново въвеждане на админ кода.
int adminCardFails = 0;
const int ADMIN_CARD_MAX_FAILS = 3;

// Автоматично излизане от админ режим при бездействие
unsigned long adminLastActivity = 0;
const unsigned long ADMIN_TIMEOUT = 60000;   // 1 минута без активност

// Потвърждение на фабричното нулиране - второ натискане на 'D'
bool wipePending = false;
unsigned long wipeAskedAt = 0;
const unsigned long WIPE_CONFIRM_WINDOW = 5000;   // 5 s за повторно натискане на D

// Състояние на бутона за излизане отвътре (виж handleExitButton)
const unsigned long REX_DEBOUNCE = 50;  // време за филтриране на дребезга, ms
int  rexStable   = HIGH;                // последно потвърдено ниво (HIGH = отпуснат)
int  rexLast     = HIGH;                // последно прочетено ниво (може още да "трепти")
unsigned long rexChangedAt = 0;         // кога нивото се е променило за последно

// ================= ДИСПЛЕЙ / ПОТРЕБИТЕЛСКИ ИНТЕРФЕЙС =================
// При rotation 1 екранът е 320x240. Вграденият шрифт е 6*size пиксела
// широк и 8*size пиксела висок - оттам идват сметките за центриране.

enum UiScreen { UI_NONE, UI_IDLE, UI_PIN, UI_ADMIN_CARD, UI_ADMIN, UI_LOCK };

UiScreen uiCurrent   = UI_NONE; // текущо показан екран
bool     uiForce     = true;    // заявка за пълно преначертаване
int      uiShownLen  = -1;      // последно начертана дължина на въведеното
long     uiShownSecs = -1;      // последно начертана стойност на обратното броене
unsigned long uiHoldUntil = 0;  // задържа съобщение на екрана до този момент

// Центриран текст, изчертан върху собствен фон
void uiText(const char *s, int y, uint8_t size, uint16_t fg, uint16_t bg) {
    tft.setTextSize(size);
    tft.setTextColor(fg, bg);
    int w = strlen(s) * 6 * size;
    tft.setCursor((320 - w) / 2, y);
    tft.print(s);
}

// Центриран текст върху изчистена лента - за стойности с променлива дължина
void uiLine(const char *s, int y, uint8_t size, uint16_t fg) {
    tft.fillRect(0, y, 320, 8 * size, ILI9341_BLACK);
    uiText(s, y, size, fg, ILI9341_BLACK);
}

// Заглавна лента в горната част на екрана
void uiBar(const char *title, uint16_t color) {
    tft.fillRect(0, 0, 320, 34, color);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE, color);
    tft.setCursor(12, 10);
    tft.print(title);
}

// Четири квадратчета - по една запълнена точка за всяка въведена цифра.
// Самите цифри никога не се показват на екрана.
void uiPinSlots(int filled) {
    const int n = 4, w = 34, h = 46, gap = 18, y = 105;
    int x0 = (320 - (n * w + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (w + gap);
        tft.fillRect(x, y, w, h, ILI9341_BLACK);
        tft.drawRect(x, y, w, h, ILI9341_DARKGREY);
        if (i < filled) tft.fillCircle(x + w / 2, y + h / 2, 9, ILI9341_CYAN);
    }
}

// Съобщение на цял екран, което остава поне holdMs милисекунди
void uiFlash(const char *l1, const char *l2, uint16_t bg, unsigned long holdMs) {
    tft.fillScreen(bg);
    uiText(l1, 85, 3, ILI9341_WHITE, bg);
    if (l2) uiText(l2, 135, 2, ILI9341_WHITE, bg);
    uiHoldUntil = millis() + holdMs;
    uiForce = true;
}

// Начален екран - системата е заключена и чака карта или код
void uiScreenIdle() {
    tft.fillScreen(ILI9341_BLACK);
    uiBar("SMART LOCK", ILI9341_NAVY);
    uiText("LOCKED", 95, 4, ILI9341_WHITE, ILI9341_BLACK);
    uiText("Scan card or enter PIN", 155, 2, ILI9341_LIGHTGREY, ILI9341_BLACK);
    uiText("# confirm   B back   C or * clear", 205, 1, ILI9341_DARKGREY, ILI9341_BLACK);
}

// Екран при въвеждане на код
void uiScreenPin() {
    tft.fillScreen(ILI9341_BLACK);
    uiBar("ENTER PIN", ILI9341_NAVY);
    uiText("# confirm   B back   C or * clear", 205, 1, ILI9341_DARKGREY, ILI9341_BLACK);
    uiPinSlots(0);
}

// Екран на админ режима - статичната част с подсказките за клавишите
void uiScreenAdmin() {
    tft.fillScreen(ILI9341_BLACK);
    uiBar("ADMIN MODE", ILI9341_ORANGE);
    uiText("Scan a card to add or remove it", 55, 1, ILI9341_LIGHTGREY, ILI9341_BLACK);
    uiText("Admin code + D twice = erase all", 170, 1, ILI9341_RED, ILI9341_BLACK);
    uiText("Type 4 digits, then A add / D delete", 190, 1, ILI9341_DARKGREY, ILI9341_BLACK);
    uiText("B back   C clear   # exit   * storage", 206, 1, ILI9341_DARKGREY, ILI9341_BLACK);
}

// Динамичната част на админ екрана - брой записи и напредък на въвеждането.
// "+M" означава главната карта, която е зашита в кода и не се брои във вектора.
void uiAdminStatus(int len) {
    char buf[40];
    snprintf(buf, sizeof(buf), "Cards: %u+M   Codes: %u",
             (unsigned)authorizedCards.size(), (unsigned)authorizedPasswords.size());
    uiLine(buf, 95, 2, ILI9341_WHITE);

    if (len == 0) snprintf(buf, sizeof(buf), "Ready");
    else          snprintf(buf, sizeof(buf), "CODE  %d/4", len);
    uiLine(buf, 145, 2, ILI9341_ORANGE);
}

// Екран на втората стъпка от админ входа - чакаме главната карта.
// Статичната част; оставащите секунди се дописват от uiTick().
void uiScreenAdminCard() {
    tft.fillScreen(ILI9341_BLACK);
    uiBar("ADMIN LOGIN", ILI9341_ORANGE);
    uiText("STEP 2 OF 2", 55, 2, ILI9341_WHITE, ILI9341_BLACK);
    uiText("Scan admin card", 90, 2, ILI9341_LIGHTGREY, ILI9341_BLACK);
    uiText("C or * cancel", 205, 1, ILI9341_DARKGREY, ILI9341_BLACK);
}

// Екран при блокирана клавиатура
void uiScreenLock() {
    tft.fillScreen(ILI9341_BLACK);
    uiBar("LOCKED OUT", ILI9341_MAROON);
    uiText("TOO MANY ATTEMPTS", 65, 2, ILI9341_RED, ILI9341_BLACK);
    uiText("Use a card to unlock", 205, 2, ILI9341_LIGHTGREY, ILI9341_BLACK);
}

// Решава кой екран трябва да се вижда и преначертава само това,
// което наистина се е променило (иначе дисплеят видимо мига).
void uiTick() {
    if (millis() < uiHoldUntil) return;      // още се показва съобщение от uiFlash()

    // Приоритет на състоянията:
    // блокиране > чакане на админ карта > админ > въвеждане > начален екран
    UiScreen want;
    if (isKeypadLocked)                 want = UI_LOCK;
    else if (isAdminPending)            want = UI_ADMIN_CARD;
    else if (isAdminMode)               want = UI_ADMIN;
    else if (inputBuffer.length() > 0)  want = UI_PIN;
    else                                want = UI_IDLE;

    // Смяна на екрана - изчертаваме статичната част наново
    if (uiForce || want != uiCurrent) {
        uiForce = false;
        uiCurrent = want;
        uiShownLen = -1;
        uiShownSecs = -1;
        switch (want) {
            case UI_IDLE:       uiScreenIdle();      break;
            case UI_PIN:        uiScreenPin();       break;
            case UI_ADMIN_CARD: uiScreenAdminCard(); break;
            case UI_ADMIN:      uiScreenAdmin();     break;
            case UI_LOCK:       uiScreenLock();      break;
            default: break;
        }
    }

    // Обновяване само на динамичните части на текущия екран
    int len = inputBuffer.length();

    if (want == UI_PIN && len != uiShownLen) {
        uiShownLen = len;
        uiPinSlots(len);
    }
    else if (want == UI_ADMIN && len != uiShownLen) {
        uiShownLen = len;
        uiAdminStatus(len);
    }
    else if (want == UI_ADMIN_CARD) {
        // Оставащи секунди до изтичане на прозореца за картата.
        // Тук закръгляме нагоре (+999), а не с +1 както при блокирането:
        // така броенето тръгва точно от 15 и всяко число стои по 1 s.
        long rem = (long)ADMIN_CARD_TIMEOUT - (long)(millis() - adminCardWaitStart);
        if (rem < 0) rem = 0;
        long left = (rem + 999) / 1000;
        if (left != uiShownSecs) {
            uiShownSecs = left;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld s", left);
            uiLine(buf, 130, 4, ILI9341_ORANGE);
        }
    }
    else if (want == UI_LOCK) {
        // Оставащи секунди до отблокиране, закръглени нагоре (+1),
        // за да не стои "0 s" през цялата последна секунда
        long left = ((long)LOCKOUT_DURATION - (long)(millis() - lockoutStartTime)) / 1000 + 1;
        if (left < 0) left = 0;
        if (left != uiShownSecs) {
            uiShownSecs = left;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld s", left);
            uiLine(buf, 115, 4, ILI9341_RED);
        }
    }
}

// --- ФУНКЦИИ ЗА СИГНАЛИЗАЦИЯ ---

// Разрешен достъп: зеленият светодиод свети, зумерът звучи непрекъснато
// и бравата стои отключена 2 секунди
void signalAccessGranted() {
    Serial.println("Access Granted.");

    uiFlash("ACCESS GRANTED", "Door open", ILI9341_DARKGREEN, 500);

    digitalWrite(LED_OK, HIGH);
    digitalWrite(BUZZER, HIGH);
    digitalWrite(LOCK, HIGH);
    delay(2000);
    digitalWrite(LOCK, LOW);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_OK, LOW);
}

// Отказан достъп: две къси червени примигвания със звук
void signalAccessDenied() {
    Serial.println("Access Denied.");

    uiFlash("ACCESS DENIED", NULL, ILI9341_MAROON, 1500);

    digitalWrite(LED_ERR, HIGH);
    digitalWrite(BUZZER, HIGH);
    delay(100);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_ERR, LOW);
    delay(100);
    digitalWrite(LED_ERR, HIGH);
    digitalWrite(BUZZER, HIGH);
    delay(100);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_ERR, LOW);
}

// Успешно действие в админ режим - два кратки сигнала
void signalAdminSuccess() {
    digitalWrite(BUZZER, HIGH); delay(100); digitalWrite(BUZZER, LOW);
    delay(50);
    digitalWrite(BUZZER, HIGH); delay(100); digitalWrite(BUZZER, LOW);
}

// Грешка в админ режим - един дълъг сигнал
void signalAdminError() {
    digitalWrite(BUZZER, HIGH); delay(1000); digitalWrite(BUZZER, LOW);
}

// Същите изходи като при "разрешен достъп", но екранът не се пипа:
// панелът гледа към улицата и никой отвън не трябва да разбира,
// че някой току-що е излязъл.
void openFromInside() {
    Serial.println("\nREX: exit button - door open.");

    digitalWrite(LED_OK, HIGH);
    digitalWrite(BUZZER, HIGH);
    digitalWrite(LOCK,   HIGH);
    delay(2000);
    digitalWrite(LOCK,   LOW);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_OK, isAdminMode ? HIGH : LOW);   // ако сме в админ режим, зеленият светодиод остава светнат
}

// Чете бутона за излизане с филтриране на дребезга и реагира само
// на падащия фронт (натискане), не и на задържане или отпускане.
void handleExitButton() {
    int reading = digitalRead(REX_BTN);

    if (reading != rexLast) {           // нивото се мени - започваме отброяването отначало
        rexLast = reading;
        rexChangedAt = millis();
        return;
    }
    if (millis() - rexChangedAt < REX_DEBOUNCE) return;   // още не се е успокоило
    if (reading == rexStable) return;                     // няма нищо ново

    rexStable = reading;
    if (rexStable == LOW) openFromInside();               // падащ фронт = натискане
}

// --- ФУНКЦИИ ЗА СЛЕДЕНЕ НА ФЛАШ ПАМЕТТА (NVS) ---
// Дефинирани тук, преди setup() и loop(), за да няма проблеми с компилацията.

// Брой записи, заети само от нашето NVS пространство
size_t nsUsedEntries() {
    nvs_handle_t h;
    size_t used = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_used_entry_count(h, &used);
        nvs_close(h);
    }
    return used;
}

// Кратък едноредов отчет - при стартиране и след всяко добавяне или премахване
void printStorageShort() {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        Serial.println("NVS: stats error");
        return;
    }
    size_t ns = nsUsedEntries();
    int delta = (int)ns - (int)lastNsUsed;   // с колко записа сме се променили от миналия път
    lastNsUsed = ns;

    Serial.printf("STORAGE: %u/%u entries (%.1f%%) | smart-lock: %u (%+d) | Cards: %u+M | Pass: %u\n",
        (unsigned)st.used_entries, (unsigned)st.total_entries,
        100.0f * st.used_entries / st.total_entries,
        (unsigned)ns, delta,
        (unsigned)authorizedCards.size(),
        (unsigned)authorizedPasswords.size());
}

// Пълен отчет за паметта, извиква се с '*' в админ режим
void printStorageStats() {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        Serial.println("NVS: stats error");
        uiFlash("STORAGE", "Read error", ILI9341_MAROON, 1500);
        signalAdminError();
        return;
    }
    size_t ns = nsUsedEntries();
    int pct = (100 * st.used_entries) / st.total_entries;

    // Текстова лента за запълването: 20 знака, всеки по 5%
    char bar[21];
    for (int i = 0; i < 20; i++) bar[i] = (i < pct / 5) ? '#' : '-';
    bar[20] = '\0';

    Serial.println("\n--- FLASH (NVS) STORAGE ---");
    Serial.printf("Used       : %u / %u entries (%u / %u bytes)\n",
        (unsigned)st.used_entries, (unsigned)st.total_entries,
        (unsigned)st.used_entries * NVS_ENTRY_SIZE,
        (unsigned)st.total_entries * NVS_ENTRY_SIZE);
    Serial.printf("Free       : %u entries (%u bytes)\n",
        (unsigned)st.free_entries, (unsigned)st.free_entries * NVS_ENTRY_SIZE);
    Serial.printf("[%s] %d%%\n", bar, pct);
    Serial.printf("Namespaces : %u (this app uses %u entries)\n",
        (unsigned)st.namespace_count, (unsigned)ns);
    Serial.printf("Stored     : %u cards (+1 built-in), %u passwords\n",
        (unsigned)authorizedCards.size(), (unsigned)authorizedPasswords.size());
    // Груба оценка: една карта/парола заема около 2 записа
    Serial.printf("Room for   : ~%u more cards / ~%u more passwords\n",
        (unsigned)(st.free_entries / 2), (unsigned)(st.free_entries / 2));
    Serial.println("---------------------------");
    char scr[24];
    snprintf(scr, sizeof(scr), "%d%% used", pct);
    uiFlash("STORAGE", scr, ILI9341_NAVY, 2000);
    signalAdminSuccess();
}

// --- УПРАВЛЕНИЕ НА ПАМЕТТА ---

// Записва всички карти в NVS под ключове "c_0", "c_1", ...
// Форматът на един запис е: [дължина][UID байтове]
void saveCards() {
    int oldCount = preferences.getInt("c_count", 0);
    for (int i = 0; i < authorizedCards.size(); i++) {
        String key = "c_" + String(i);
        byte buffer[11];
        buffer[0] = authorizedCards[i].size;
        memcpy(&buffer[1], authorizedCards[i].uid, authorizedCards[i].size);
        preferences.putBytes(key.c_str(), buffer, authorizedCards[i].size + 1);
    }
    // Изтриваме ключовете, останали от по-дълъг предишен списък
    for (int i = authorizedCards.size(); i < oldCount; i++) {
        String key = "c_" + String(i);
        preferences.remove(key.c_str());
    }
    preferences.putInt("c_count", authorizedCards.size());
}

// Записва всички допълнителни пароли в NVS под ключове "p_0", "p_1", ...
void savePasswords() {
    int oldCount = preferences.getInt("p_count", 0);
    for (int i = 0; i < authorizedPasswords.size(); i++) {
        String key = "p_" + String(i);
        preferences.putString(key.c_str(), authorizedPasswords[i]);
    }
    // Изтриваме ключовете, останали от по-дълъг предишен списък
    for (int i = authorizedPasswords.size(); i < oldCount; i++) {
        String key = "p_" + String(i);
        preferences.remove(key.c_str());
    }
    preferences.putInt("p_count", authorizedPasswords.size());
}

// Зарежда картите и паролите от NVS при стартиране.
// Повредените записи се прескачат, за да не се счупи системата.
void loadData() {
    int c_count = preferences.getInt("c_count", 0);
    authorizedCards.clear();
    for (int i = 0; i < c_count; i++) {
        String key = "c_" + String(i);
        byte buffer[11];
        size_t len = preferences.getBytes(key.c_str(), buffer, 11);
        if (len > 0) {
            CardUID card;
            card.size = buffer[0];
            if(card.size < 4 || card.size > 10) continue;   // невалидна дължина - пропускаме записа
            memcpy(card.uid, &buffer[1], card.size);
            authorizedCards.push_back(card);
        }
    }

    int p_count = preferences.getInt("p_count", 0);
    authorizedPasswords.clear();
    for (int i = 0; i < p_count; i++) {
        String key = "p_" + String(i);
        String pass = preferences.getString(key.c_str(), "");
        if (pass.length() == 4) {   // приемаме само кодове с дължина 4
            authorizedPasswords.push_back(pass);
        }
    }
    Serial.print("Loaded Cards: "); Serial.println(c_count);
    Serial.print("Loaded Passwords: "); Serial.println(p_count);
}

// --- ПОМОЩНИ ФУНКЦИИ ---

// Отпечатва всички валидни пароли по серийния порт (само в админ режим)
void printAllPasswords() {
    Serial.println("\n--- LIST OF PASSWORDS ---");
    Serial.println("Master: " + masterPassword);   // главната парола е винаги валидна
    Serial.println("Master card: built-in (cannot be removed)");

    if (authorizedPasswords.size() == 0) {
        Serial.println("(No custom passwords stored)");
    } else {
        for (int i = 0; i < authorizedPasswords.size(); i++) {
            Serial.print("Custom ");
            Serial.print(i + 1);
            Serial.print(": ");
            Serial.println(authorizedPasswords[i]);
        }
    }
    Serial.println("-------------------------");
}

// Сравнява два UID-а байт по байт (дължината се проверява от викащия)
bool compareUID(byte *u1, byte *u2, byte size) {
    for (byte i = 0; i < size; i++) if (u1[i] != u2[i]) return false;
    return true;
}

// Проверява дали сканираната карта е зашитата в кода главна карта
bool isMasterCard(byte *uid, byte size) {
    return size == MASTER_UID_LEN && compareUID((byte *)MASTER_UID, uid, size);
}

// Връща индекса на картата в списъка с динамичните карти или -1, ако не е
// намерена. Главната карта не е в този списък - за нея виж isMasterCard().
int findCardIndex(byte *uid, byte size) {
    for (int i = 0; i < authorizedCards.size(); i++) {
        if (authorizedCards[i].size == size && compareUID(authorizedCards[i].uid, uid, size)) return i;
    }
    return -1;
}

// Проверява дали кодът е сред допълнителните пароли
bool checkExtraPassword(String pass) {
    for (String s : authorizedPasswords) {
        if (s == pass) return true;
    }
    return false;
}

// Отблокира клавиатурата предсрочно - при поднесена валидна карта
void unlockSystem() {
    isKeypadLocked = false;
    failedAttempts = 0;
    digitalWrite(LED_ERR, LOW);
    Serial.println("System Unlocked via Card!");
}

// Отчита един неуспешен опит и блокира клавиатурата след третия пореден.
// Общият брояч е същият, който следи и грешните PIN кодове.
void registerFailedAttempt() {
    failedAttempts++;
    if (failedAttempts >= 3) {
        isKeypadLocked = true;
        lockoutStartTime = millis();
        Serial.println("SYSTEM LOCKED! (Only Card can unlock)");
        digitalWrite(BUZZER, HIGH); delay(1000); digitalWrite(BUZZER, LOW);
    }
}

// Затваря прозореца за админ карта и се връща в нормален режим.
// Това е единственото място, където се начислява обща грешка, за да не
// може един прозорец да бъде таксуван два пъти (например трета грешна
// карта, последвана от изтичане на времето). Прозорец, в който е била
// поднесена поне една грешна карта, струва точно една обща грешка -
// независимо дали е прекратен от изчерпани опити, от време или ръчно.
void endAdminLogin(const char *l1, const char *l2, uint16_t bg) {
    bool sawWrongCard = (adminCardFails > 0);

    isAdminPending = false;
    adminCardFails = 0;
    inputBuffer = "";
    uiFlash(l1, l2, bg, 1500);

    if (sawWrongCard) registerFailedAttempt();
}

// Двете стъпки са минати успешно - вече наистина влизаме в админ режим
void enterAdminMode() {
    isAdminPending = false;
    isAdminMode = true;
    adminLastActivity = millis();
    adminCardFails = 0;                 // успешното влизане нулира двата брояча -
    failedAttempts = 0;                 // и админ опитите, и общите грешки
    inputBuffer = "";

    Serial.println("\nAdmin Mode ON (code + admin card, Green LED).");
    digitalWrite(LED_OK, HIGH);
    uiFlash("ADMIN OK", "Card accepted", ILI9341_DARKGREEN, 1500);
    digitalWrite(BUZZER, HIGH); delay(500); digitalWrite(BUZZER, LOW);

    // Показваме всички валидни пароли по серийния порт
    printAllPasswords();
}

// Фабрично нулиране: изтрива всички добавени карти и кодове.
// Главната карта и зашитите пароли продължават да работят.
void wipeAll() {
    authorizedCards.clear();
    authorizedPasswords.clear();
    preferences.clear();          // изтрива цялото пространство "smart-lock"

    Serial.println("\nADMIN: FACTORY RESET - all cards and codes erased.");
    Serial.println("Built-in master card and hardcoded codes still work.");

    printAllPasswords();
    printStorageShort();
}

void setup() {
    // Изходи към бравата, зумера и светодиодите; вход за бутона отвътре
    pinMode(LOCK, OUTPUT);
    pinMode(BUZZER, OUTPUT);
    pinMode(LED_OK, OUTPUT);
    pinMode(LED_ERR, OUTPUT);
    pinMode(REX_BTN, INPUT_PULLUP);     // бутонът отвътре е активен при LOW

    // Всичко започва в изключено състояние - бравата остава заключена
    digitalWrite(LOCK, LOW);
    digitalWrite(BUZZER, LOW);
    digitalWrite(LED_OK, LOW);
    digitalWrite(LED_ERR, LOW);

    Serial.begin(115200);
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);

    // Дисплей: начален екран, докато системата зарежда
    tft.begin(27000000);
    tft.setRotation(1);              // 320x240, landscape
    tft.fillScreen(ILI9341_BLACK);
    uiText("SMART LOCK", 100, 3, ILI9341_WHITE, ILI9341_BLACK);
    uiText("starting...", 145, 2, ILI9341_DARKGREY, ILI9341_BLACK);
    uiHoldUntil = millis() + 2500;   // задържаме началния екран 2.5 s

    mfrc522.PCD_Init();

    // Отваряме NVS пространството и зареждаме запазените карти и кодове
    preferences.begin("smart-lock", false);
    loadData();

    // Базова стойност за разликите + първоначален отчет при всяко стартиране
    lastNsUsed = nsUsedEntries();
    printStorageShort();

    // REX: тръгваме от реалното ниво на бутона, за да не бъде отчетен
    // като ново натискане бутон, който вече е задържан при включване
    rexLast = rexStable = digitalRead(REX_BTN);
    rexChangedAt = millis();

    // След изтичането на началния екран първият uiTick() рисува основния екран
    uiForce = true;
}

void loop() {
    uiTick();
    handleExitButton();        // <-- тук, преди всичко, което може да прекъсне цикъла по-рано

    // 1. СЪСТОЯНИЕ "ЗАКЛЮЧЕН"
    if (isKeypadLocked) {
        // Червеният светодиод мига на всеки 500 ms, докато трае блокирането
        if ((millis() / 500) % 2 == 0) digitalWrite(LED_ERR, HIGH);
        else digitalWrite(LED_ERR, LOW);

        if (millis() - lockoutStartTime >= LOCKOUT_DURATION) {
            isKeypadLocked = false;
            failedAttempts = 0;
            digitalWrite(LED_ERR, LOW);
            Serial.println("Time elapsed. System Unlocked.");
        }
    }

    // 2. ТАЙМ-АУТ НА ВЪВЕЖДАНЕТО
    if (inputBuffer.length() > 0 && (millis() - lastKeyPressTime > INPUT_TIMEOUT)) {
        Serial.println("\nTimeout!");
        inputBuffer = "";
        if(!isAdminMode) signalAccessDenied();
    }
    // 2a. ТАЙМ-АУТ НА ВТОРАТА СТЪПКА ОТ АДМИН ВХОДА
    // Крайният срок е абсолютен - 15 s от въвеждането на кода. Сканирането
    // на карта не го удължава. Само по себе си изтичането на времето не е
    // грешка; обща грешка се начислява само ако е имало сгрешена карта.
    if (isAdminPending && (millis() - adminCardWaitStart > ADMIN_CARD_TIMEOUT)) {
        Serial.println("\nADMIN: no admin card in time - login window closed.");
        endAdminLogin("LOGIN TIMEOUT", "No admin card", ILI9341_MAROON);
    }

    // 2b. ТАЙМ-АУТ НА АДМИН РЕЖИМА
    if (isAdminMode && (millis() - adminLastActivity > ADMIN_TIMEOUT)) {
        isAdminMode = false;
        inputBuffer = "";
        // Заявката за нулиране не бива да преживява сесията, която я е
        // подала - иначе следващият админ може да довърши с едно 'D'
        // потвърждение, което никога не е виждал на екрана.
        wipePending = false;
        digitalWrite(LED_OK, LOW);
        Serial.println("\nADMIN: Timeout - leaving admin mode.");
        uiFlash("ADMIN TIMEOUT", "Signed out", ILI9341_ORANGE, 1500);
    }

    // 3. RFID ЛОГИКА
    // Проверяваме за карта само ако в момента не се въвежда парола
    if (inputBuffer.length() == 0) {
        if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
            int index = findCardIndex(mfrc522.uid.uidByte, mfrc522.uid.size);

            if (isAdminPending) {
                // Втора стъпка от админ входа: приема се само главната карта.
                // Обикновена потребителска карта тук не отваря вратата -
                // иначе двустепенната проверка би се заобикаляла.
                if (isMasterCard(mfrc522.uid.uidByte, mfrc522.uid.size)) {
                    enterAdminMode();
                } else {
                    // Всяка друга карта - включително валидна потребителска -
                    // е грешна за тази стъпка и не отваря вратата.
                    adminCardFails++;
                    Serial.printf("\nADMIN: wrong card (%d/%d).\n",
                                  adminCardFails, ADMIN_CARD_MAX_FAILS);

                    if (adminCardFails >= ADMIN_CARD_MAX_FAILS) {
                        // Изчерпани опити - прозорецът се затваря предсрочно
                        signalAdminError();   // първо звукът, после съобщението
                        endAdminLogin("LOGIN FAILED", "3 wrong cards", ILI9341_MAROON);
                    } else {
                        // Остават опити: прозорецът продължава, а обратното
                        // броене върви - затова тук е само кратък сигнал,
                        // а не дългият звук на signalAdminError().
                        char left[24];
                        snprintf(left, sizeof(left), "%d of %d left",
                                 ADMIN_CARD_MAX_FAILS - adminCardFails,
                                 ADMIN_CARD_MAX_FAILS);
                        digitalWrite(BUZZER, HIGH); delay(200); digitalWrite(BUZZER, LOW);
                        uiFlash("WRONG CARD", left, ILI9341_MAROON, 1200);
                    }
                }
            }
            else if (isAdminMode) {
                adminLastActivity = millis();          // сканирането също се брои за активност

                if (isMasterCard(mfrc522.uid.uidByte, mfrc522.uid.size)) {
                    // Главната карта не може да се изтрие - тя е последният начин за достъп
                    Serial.println("ADMIN: Master card is built-in - cannot be changed.");
                    uiFlash("MASTER CARD", "Built-in, cannot change", ILI9341_ORANGE, 1500);
                    signalAdminError();
                }
                else if (index != -1) {
                    // Позната карта в админ режим -> премахваме я
                    authorizedCards.erase(authorizedCards.begin() + index);
                    saveCards();
                    Serial.println("ADMIN: Card REMOVED.");
                    uiFlash("CARD REMOVED", NULL, ILI9341_ORANGE, 1500);
                    printStorageShort();
                    signalAdminSuccess();
                } else {
                    // Непозната карта в админ режим -> добавяме я
                    CardUID newCard;
                    newCard.size = mfrc522.uid.size;
                    memcpy(newCard.uid, mfrc522.uid.uidByte, mfrc522.uid.size);
                    authorizedCards.push_back(newCard);
                    saveCards();
                    Serial.println("ADMIN: Card ADDED.");
                    uiFlash("CARD ADDED", NULL, ILI9341_DARKGREEN, 1500);
                    printStorageShort();
                    signalAdminSuccess();
                }
            } else {
                // Нормален режим: валидната карта отключва и вдига блокирането
                if (index != -1 || isMasterCard(mfrc522.uid.uidByte, mfrc522.uid.size)){
                    unlockSystem();
                    signalAccessGranted();
                } else {
                    Serial.println("Unknown Card.");
                    signalAccessDenied();
                }
            }
            // Приспиваме картата, за да не се чете многократно
            mfrc522.PICC_HaltA();
            mfrc522.PCD_StopCrypto1();
        }
    }

    // 4. ЛОГИКА НА КЛАВИАТУРАТА
    if (isKeypadLocked) return;   // при блокиране клавиатурата е напълно неактивна

    char key = kpd.getKey();
    if (key) {
        lastKeyPressTime = millis();
        adminLastActivity = millis();
        digitalWrite(BUZZER, HIGH); delay(50); digitalWrite(BUZZER, LOW);   // звук при натискане
        Serial.print(key);

        // --- ВТОРА СТЪПКА ОТ АДМИН ВХОДА ---
        // Докато чакаме картата, клавиатурата приема само отказ. Ако
        // допускахме цифри, буферът нямаше да е празен и RFID четецът
        // изобщо нямаше да бъде проверяван (виж точка 3 по-горе).
        if (isAdminPending) {
            if (key == 'C' || key == '*') {
                // Отказът сам по себе си не е грешка. Ако обаче вече е била
                // поднесена сгрешена карта, прозорецът пак си остава платен -
                // отказът не може да служи за бягство от начислената грешка.
                Serial.println("\nADMIN: login cancelled.");
                endAdminLogin("LOGIN CANCELLED", NULL, ILI9341_ORANGE);
            }
        }

        // --- ЛОГИКА В АДМИН РЕЖИМ ---
        else if (isAdminMode) {

            // Изричен отказ на нулирането - същите клавиши както при админ
            // входа. Състоянието се запомня тук, защото редът отдолу го
            // изчиства, а искаме и да "изядем" клавиша: иначе '*' би
            // отказал нулирането, но пак би отворил отчета за паметта.
            bool cancelWipe = wipePending && (key == 'C' || key == '*');

            if (key != 'D') wipePending = false;   // всеки друг клавиш отказва нулирането

            if (cancelWipe) {
                inputBuffer = "";
                Serial.println("\nADMIN: wipe cancelled.");
                uiFlash("WIPE CANCELLED", NULL, ILI9341_ORANGE, 1500);
                signalAdminSuccess();
            }
            else if (key == '#') {
                // Изход от админ режим
                isAdminMode = false;
                Serial.println("\nExiting Admin Mode.");
                digitalWrite(LED_OK, LOW);
                inputBuffer = "";
            }
            else if (key == 'C'){
                // Изчистване на въведеното
                inputBuffer = "";
                Serial.println("\nCleared.");
            }
            else if (key == 'B'){
                // Изтриване на последната цифра
                if (inputBuffer.length() > 0){
                    inputBuffer.remove(inputBuffer.length() - 1);
                }
                Serial.println("\nBack");
            }
            else if (key == '*'){
                // Пълен отчет за флаш паметта
                Serial.println();
                printStorageStats();
            }
            else if (key == 'A'){
                // Добавяне на нова парола
                if (inputBuffer.length() != 4){
                    Serial.println("\nADMIN: Type 4 digits first. ");
                    uiFlash("NEED 4 DIGITS", "Then press A", ILI9341_MAROON, 1500);
                    signalAdminError();
                }
                else if (inputBuffer == masterPassword ||
                    inputBuffer == adminPassword ||
                    checkExtraPassword(inputBuffer)){
                    // Кодът вече е зает - не допускаме дубликати
                    Serial.println("\nADMIN: Code already exists.");
                    uiFlash("CODE EXISTS", "Not added", ILI9341_MAROON, 1500);
                    signalAdminError();
                }
                else{
                    authorizedPasswords.push_back(inputBuffer);
                    savePasswords();
                    Serial.println("\nADMIN: Password ADDED: " + inputBuffer);
                    printStorageShort();
                    uiFlash("CODE ADDED", NULL, ILI9341_DARKGREEN, 1500);
                    signalAdminSuccess();
                    printAllPasswords();
                }
                inputBuffer = "";
            }
            else if (key == 'D') {
                // Второ 'D' в рамките на прозореца за потвърждение -> изтриваме всичко
                if (wipePending && (millis() - wipeAskedAt <= WIPE_CONFIRM_WINDOW)) {
                    wipePending = false;
                    wipeAll();
                    uiFlash("WIPED", "Master card built-in", ILI9341_MAROON, 2000);
                    signalAdminSuccess();
                }
                // Админ парола + D -> искаме потвърждение
                else if (inputBuffer == adminPassword) {
                    wipePending = true;
                    wipeAskedAt = millis();
                    Serial.println("\nADMIN: WIPE ALL? D again = confirm, C or * = cancel.");
                    uiFlash("WIPE ALL?", "D confirm  C or * cancel", ILI9341_MAROON, 4000);
                }
                else if (inputBuffer.length() != 4) {
                    Serial.println("\nADMIN: Type 4 digits first.");
                    uiFlash("NEED 4 DIGITS", "Then press D", ILI9341_MAROON, 1500);
                    signalAdminError();
                }
                else {
                    // Иначе 'D' изтрива въведената парола, ако тя съществува
                    bool found = false;
                    for (int i = 0; i < authorizedPasswords.size(); i++) {
                        if (authorizedPasswords[i] == inputBuffer) {
                            authorizedPasswords.erase(authorizedPasswords.begin() + i);
                            found = true;
                            break;
                        }
                    }
                    if (found) {
                        savePasswords();
                        Serial.println("\nADMIN: Password REMOVED: " + inputBuffer);
                        printStorageShort();
                        uiFlash("CODE REMOVED", NULL, ILI9341_ORANGE, 1500);
                        signalAdminSuccess();
                        printAllPasswords();
                    } else {
                        Serial.println("\nADMIN: Password not found.");
                        uiFlash("CODE NOT FOUND", NULL, ILI9341_MAROON, 1500);
                        signalAdminError();
                    }
                }
                inputBuffer = "";
            }
            else {                              // цифри
                if (inputBuffer.length() < 4) {
                    inputBuffer += key;
                } else {
                    Serial.println("\nError: Too long.");
                    uiFlash("TOO LONG", "Codes are 4 digits", ILI9341_MAROON, 1500);
                    signalAdminError();
                    inputBuffer = "";
                }
            }
        }

        // --- ЛОГИКА В НОРМАЛЕН РЕЖИМ ---
        else {
            if (key == '#') {
                // Потвърждаване на въведения код
                Serial.println();
                if (inputBuffer.length() == 0) {
                    Serial.println("Nothing entered - ignored.");
                }
                else if (inputBuffer == masterPassword || checkExtraPassword(inputBuffer)) {
                    // Валиден код -> отваряме вратата
                    signalAccessGranted();
                    inputBuffer = "";
                    failedAttempts = 0;
                }
                else if (inputBuffer == adminPassword) {
                    // Админ код -> само първа стъпка. Правата се дават чак
                    // след като бъде поднесена главната карта (виж точка 3).
                    isAdminPending = true;
                    adminCardWaitStart = millis();
                    adminCardFails = 0;      // всеки нов прозорец започва с 3 опита
                    inputBuffer = "";
                    Serial.println("Admin code OK - waiting for admin card (15 s).");
                    digitalWrite(BUZZER, HIGH); delay(150); digitalWrite(BUZZER, LOW);
                }
                else {
                    // Грешен код -> след 3 поредни грешки блокираме клавиатурата
                    inputBuffer = "";
                    signalAccessDenied();
                    failedAttempts++;
                    if (failedAttempts >= 3) {
                        isKeypadLocked = true;
                        lockoutStartTime = millis();
                        Serial.println("SYSTEM LOCKED! (Only Card can unlock)");
                        digitalWrite(BUZZER, HIGH); delay(1000); digitalWrite(BUZZER, LOW);
                    }
                }
            }
            else if (key == '*' || key == 'C') {
                // Изчистване на въведеното
                inputBuffer = "";
                Serial.println("\nCleared.");
            }
            else if (key == 'B'){
                // Изтриване на последната цифра
                if(inputBuffer.length()>0){
                    inputBuffer.remove(inputBuffer.length() - 1);
                }
                Serial.println("\nBack.");
            }
            else if (key == 'A' || key == 'D'){
                //  Няма логика за това в нормален режим, само в админ.
            }
            else {                              // цифри
                if (inputBuffer.length() < 4) {
                    inputBuffer += key;
                } else {
                    // Повече от 4 цифри се третира като грешен опит
                    Serial.println("\nError: >4 digits!");
                    inputBuffer = "";
                    signalAccessDenied();
                    failedAttempts++;
                    if (failedAttempts >= 3) {
                        isKeypadLocked = true;
                        lockoutStartTime = millis();
                        Serial.println("SYSTEM LOCKED!");
                        digitalWrite(BUZZER, HIGH); delay(1000); digitalWrite(BUZZER, LOW);
                    }
                }
            }
        }
    }
}
