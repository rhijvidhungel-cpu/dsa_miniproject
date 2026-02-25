
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "raylib.h"
#include <openssl/sha.h>
#include "sqlite3.h"

// ---------------- CONFIG ----------------
#define W 1000  // wider window for maximizable feel
#define H 700
#define MAX_Z 50
#define Z_SIZE 70    // slightly bigger
#define P_SIZE 90    // slightly bigger
#define BULLET_SIZE 10 // slightly bigger
#define MAX_OBS 50

// ---------------- STRUCTS ----------------
typedef struct {
    Vector2 pos;
    int health;
    bool active;
    bool entered;
    bool isBoss;
    bool isBaby;   // identify baby zombies for separate FIFO
    bool isRedNormal;  // first zombie in normal queue
    bool isRedBaby;
} Zombie;

typedef struct Bullet {
    Vector2 pos;
    Vector2 vel;
    struct Bullet* next;
} Bullet;

typedef struct {
    Vector2 pos;
    Vector2 size;
    bool active;
    bool moving;
    float dir; // 1 or -1 (movement direction)
    bool isSpike;
    float blinkTimer; // if >0, draw blink state
} Obstacle;

// ---------------- GLOBALS ----------------
Zombie zombies[MAX_Z];
Vector2 player = {W/2, H-100};
Bullet* bHead = NULL;
int q[MAX_Z];
int babyQ[MAX_Z];
int userHighScore = 0;
int babyQStart = 0, babyQCount = 0;
int qStart = 0, qCount = 0;
int score = 0;
bool gameOver = false;
bool paused = false;
int normalZombieKills = 0;
bool bossActive = false;
int level = 1;
float zombieSpeed = 1.2f;
float spawnInterval = 2.0f;
int bossHealth = 300;
int bulletLimit = -1; // -1 unlimited, from level2 limited
int bulletsFired = 0;
bool gameWon = false;
    // first zombie in baby queue
Obstacle obstacles[MAX_OBS];
int numObstacles = 0;
int lastObstacleLevel = -1;

// ---------------- DATABASE ----------------
sqlite3* db;
int currentUserId = -1;
char loggedUsername[50] = "";

// ---------------- SOUND ----------------
Sound bulletSound;
Sound deadSound;
Sound bigDeadSound;
Sound refillSound;
// ---------------- QUEUE HELPERS ----------------
bool IsInQueue(int idx, int start, int count, int qArray[MAX_Z]);
void AdvanceQueue(int qArray[MAX_Z], int *qStart, int *qCount);
static inline Vector2 GetViewOffset(void){
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    float ox = (sw - W) * 0.5f;
    float oy = (sh - H) * 0.5f;
    if(ox < 0) ox = 0;
    if(oy < 0) oy = 0;
    return (Vector2){ox, oy};
}

static inline Rectangle OffsetRec(Rectangle r, Vector2 off){
    r.x += off.x;
    r.y += off.y;
    return r;
}
// ---------------- HASH ----------------
void HashPassword(const char* password, char* output){
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)password, strlen(password), hash);
    for(int i=0;i<SHA256_DIGEST_LENGTH;i++)
        sprintf(output+(i*2), "%02x", hash[i]);
    output[64]=0;
}

// ---------------- USER ----------------
bool RegisterUser(const char* username, const char* password){
    char hashed[65];
    HashPassword(password, hashed);
    sqlite3_stmt* stmt;
    if(sqlite3_prepare_v2(db,"INSERT INTO users(username,password) VALUES(?,?)",-1,&stmt,0)!=SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt,1,username,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,hashed,-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc==SQLITE_DONE;
}

bool LoginUser(const char* username, const char* password){
    char hashed[65];
    HashPassword(password, hashed);
    sqlite3_stmt* stmt;
    if(sqlite3_prepare_v2(db,"SELECT id,highscore FROM users WHERE username=? AND password=?",-1,&stmt,0)!=SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt,1,username,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,hashed,-1,SQLITE_STATIC);
    bool success=false;
    if(sqlite3_step(stmt)==SQLITE_ROW){
        currentUserId = sqlite3_column_int(stmt,0);
        userHighScore = sqlite3_column_int(stmt,1);
        score = 0;
        strcpy(loggedUsername, username);
        success = true;
    }
    sqlite3_finalize(stmt);
    return success;
}

void UpdateHighScore(int newScore){
    if(currentUserId<0) return;

    if(newScore > userHighScore) userHighScore = newScore;

    char sql[128];
    sprintf(sql,"UPDATE users SET highscore=%d WHERE id=%d", userHighScore, currentUserId);
    sqlite3_exec(db,sql,0,0,0);
}
// ---------------- BULLETS ----------------
void AddBullet(Vector2 pos){
    Bullet* b = malloc(sizeof(Bullet));
    b->pos = pos;
    b->vel = (Vector2){0,-8};
    b->next = bHead;
    bHead = b;
    PlaySound(bulletSound);
}

bool BulletHitsAnyObstacle(Vector2 p){
    for(int i=0;i<numObstacles;i++){
        if(!obstacles[i].active) continue;
        Rectangle r = (Rectangle){obstacles[i].pos.x, obstacles[i].pos.y, obstacles[i].size.x, obstacles[i].size.y};
        if(CheckCollisionPointRec(p, r)) return true;
    }
    return false;
}

void MoveBullets(){
    Bullet* c=bHead;
    Bullet* p=NULL;
    while(c){
        c->pos.x += c->vel.x;
        c->pos.y += c->vel.y;
        if(c->pos.y < 0 || BulletHitsAnyObstacle(c->pos)){
            Bullet* d = c;
            c = c->next;
            if(p) p->next = c; else bHead = c;
            free(d);
        } else {
            p = c;
            c = c->next;
        }
    }
}

// ---------------- ZOMBIES ----------------
void SpawnZombie(bool boss, Vector2 spawnPos){
    for(int i=0;i<MAX_Z;i++){
        if(!zombies[i].active){
            zombies[i].pos = (spawnPos.x>=0 && spawnPos.y>=0)
                             ? spawnPos
                             : (Vector2){GetRandomValue(40,W-40), -Z_SIZE};

            zombies[i].health = boss ? bossHealth : 50;
            zombies[i].active = true;
            zombies[i].entered = false;
            zombies[i].isBoss = boss;
            zombies[i].isBaby = false;   // IMPORTANT
            zombies[i].isRedNormal = false;
            zombies[i].isRedBaby = false;
            return;
        }
    }
}

// ---------------- UPDATED MoveZombies ----------------
void MoveZombies(){
    for(int i=0;i<MAX_Z;i++){
        if(!zombies[i].active) continue;

        Vector2 d = {player.x - zombies[i].pos.x, player.y - zombies[i].pos.y};
        float l = sqrtf(d.x*d.x + d.y*d.y);
        if(l){ d.x /= l; d.y /= l; }

        float speed = zombies[i].isBoss ? 0.5f : zombieSpeed;
        zombies[i].pos.x += d.x * speed;
        zombies[i].pos.y += d.y * speed;

        // FIFO ENTRY
        if(!zombies[i].entered && zombies[i].pos.y >= 0){
            zombies[i].entered = true;

            if(!zombies[i].isBoss){

                if(!zombies[i].isBaby){   // NORMAL QUEUE
                    if(!IsInQueue(i, qStart, qCount, q)){
                        q[(qStart+qCount)%MAX_Z] = i;
                        qCount++;
                    }
                }
                else{                     // BABY QUEUE
                    if(!IsInQueue(i, babyQStart, babyQCount, babyQ)){
                        babyQ[(babyQStart+babyQCount)%MAX_Z] = i;
                        babyQCount++;
                    }
                }
            }
        }

        Rectangle playerRec = {player.x, player.y, P_SIZE, P_SIZE};
        Rectangle zombieRec = {
            zombies[i].pos.x,
            zombies[i].pos.y,
            zombies[i].isBoss ? Z_SIZE*3 : Z_SIZE,
            zombies[i].isBoss ? Z_SIZE*3 : Z_SIZE
        };

        if(CheckCollisionRecs(playerRec, zombieRec) && !gameOver){
            gameOver = true;
            player = zombies[i].pos;
            PlaySound(deadSound);
            UpdateHighScore(score);
        }
    }

    // RESET RED FLAGS
    for(int i=0;i<MAX_Z;i++){
        zombies[i].isRedNormal = false;
        zombies[i].isRedBaby = false;
    }

    // NORMAL FIFO
    while(qCount>0 && !zombies[q[qStart]].active)
        AdvanceQueue(q,&qStart,&qCount);

    if(qCount>0)
        zombies[q[qStart]].isRedNormal = true;

    // BABY FIFO
    while(babyQCount>0 && !zombies[babyQ[babyQStart]].active)
        AdvanceQueue(babyQ,&babyQStart,&babyQCount);

    if(babyQCount>0)
        zombies[babyQ[babyQStart]].isRedBaby = true;
}
// Helper: check if zombie is already in queue
bool IsInQueue(int idx, int start, int count, int qArray[MAX_Z]){
    for(int i=0;i<count;i++){
        if(qArray[(start+i)%MAX_Z] == idx) return true;
    }
    return false;
}

// ---------------- QUEUE HELPERS ----------------
// Check if a zombie is already in the normal queue

void DrawZombies(Texture2D normalTex, Texture2D redTex, Texture2D bossTex){
    Vector2 off = GetViewOffset();
    for(int i=0;i<MAX_Z;i++){
        if(!zombies[i].active) continue;

        Texture2D tex = zombies[i].isBoss ? bossTex :
                        zombies[i].isRedNormal ? redTex :
                        zombies[i].isRedBaby ? redTex :
                        normalTex;

        float size = zombies[i].isBoss ? Z_SIZE*3 : Z_SIZE;

        DrawTexturePro(
            tex,
            (Rectangle){0,0,(float)tex.width,(float)tex.height},
            (Rectangle){zombies[i].pos.x,zombies[i].pos.y,size,size},
            (Vector2){0,0},0,WHITE
        );

        if(zombies[i].isBoss){
            DrawRectangle(
                zombies[i].pos.x,
                zombies[i].pos.y-15,
                size*(zombies[i].health/(float)bossHealth),
                10,
                RED
            );
        }
    }
}
// Advance a circular queue safely by 1
void AdvanceQueue(int qArray[MAX_Z], int *qStart, int *qCount){
    if(*qCount <= 0) return;

    int moved = 0;
    while(moved < *qCount){
        int idx = qArray[*qStart];
        if(zombies[idx].active) break; // first active found
        *qStart = (*qStart + 1) % MAX_Z;
        (*qCount)--;
        moved++;
    }

    if(*qCount <= 0) *qStart = 0; // reset if queue empty
}

// ---------------- BULLET HIT ----------------
// ---------------- UPDATED BulletHitRed ----------------
void BulletHitRed(){

    // ================= NORMAL ZOMBIE (FIFO) =================
    if(qCount>0){

        // ensure front is valid
        while(qCount>0 && !zombies[q[qStart]].active)
            AdvanceQueue(q,&qStart,&qCount);

        if(qCount>0){

            int rIndex = q[qStart];
            Zombie* red = &zombies[rIndex];

            Bullet* c = bHead;
            Bullet* p = NULL;

            while(c){

                if(CheckCollisionCircles(
                        c->pos,
                        BULLET_SIZE,
                        (Vector2){red->pos.x+Z_SIZE/2, red->pos.y+Z_SIZE/2},
                        Z_SIZE/2)){

                    red->health -= 25;

                    // remove bullet
                    Bullet* d = c;
                    c = c->next;
                    if(p) p->next = c;
                    else bHead = c;
                    free(d);

                    // if zombie dies
                    if(red->health <= 0){

                        red->active = false;
                        AdvanceQueue(q,&qStart,&qCount);

                        score += 10;
                        normalZombieKills++;

                        if(normalZombieKills % 20 == 0 && !bossActive){
                            SpawnZombie(true,(Vector2){-1,-1});
                            bossActive = true;
                        }
                    }

                    return;
                }

                p = c;
                c = c->next;
            }
        }
    }

    // ================= BOSS =================
    for(int i=0;i<MAX_Z;i++){

        if(zombies[i].active && zombies[i].isBoss){

            Bullet* c = bHead;
            Bullet* p = NULL;

            float bossSize = Z_SIZE*3;

            while(c){

                if(CheckCollisionCircles(
                        c->pos,
                        BULLET_SIZE,
                        (Vector2){zombies[i].pos.x+bossSize/2,
                                  zombies[i].pos.y+bossSize/2},
                        bossSize/2)){

                    zombies[i].health -= 25;

                    // remove bullet
                    Bullet* d = c;
                    c = c->next;
                    if(p) p->next = c;
                    else bHead = c;
                    free(d);

                    if(zombies[i].health <= 0){

                        Vector2 bossPos = zombies[i].pos;

                        zombies[i].active = false;
                        bossActive = false;

                        score += 100;
                        level++;
                        PlaySound(bigDeadSound);

                        if(level >= 7){
    gameWon = true;
    gameOver = true;
    UpdateHighScore(score);
}
                        // ================= SPAWN 5 BABIES (FIFO SAFE) =================
                        for(int k=0;k<5;k++){

                            float offsetX = -Z_SIZE*1.5f + k*(Z_SIZE*0.7f);
                            Vector2 spawnPos = {bossPos.x+offsetX, bossPos.y-50};

                            for(int j=0;j<MAX_Z;j++){

                                if(!zombies[j].active){

                                    zombies[j].pos = spawnPos;
                                    zombies[j].health = 50;
                                    zombies[j].active = true;
                                    zombies[j].entered = true;
                                    zombies[j].isBoss = false;
                                    zombies[j].isBaby = true;     // IMPORTANT

                                    zombies[j].isRedNormal = false;
                                    zombies[j].isRedBaby = false;

                                    if(!IsInQueue(j,babyQStart,babyQCount,babyQ)){
                                        babyQ[(babyQStart+babyQCount)%MAX_Z] = j;
                                        babyQCount++;
                                    }

                                    break;
                                }
                            }
                        }
                    }

                    return;
                }

                p = c;
                c = c->next;
            }
        }
    }

    // ================= BABY ZOMBIE (FIFO) =================
    if(babyQCount>0){

        while(babyQCount>0 && !zombies[babyQ[babyQStart]].active)
            AdvanceQueue(babyQ,&babyQStart,&babyQCount);

        if(babyQCount>0){

            int rIndex = babyQ[babyQStart];
            Zombie* red = &zombies[rIndex];

            Bullet* c = bHead;
            Bullet* p = NULL;

            while(c){

                if(CheckCollisionCircles(
                        c->pos,
                        BULLET_SIZE,
                        (Vector2){red->pos.x+Z_SIZE/2, red->pos.y+Z_SIZE/2},
                        Z_SIZE/2)){

                    red->health -= 25;

                    // remove bullet
                    Bullet* d = c;
                    c = c->next;
                    if(p) p->next = c;
                    else bHead = c;
                    free(d);

                    if(red->health <= 0){

                        red->active = false;
                        AdvanceQueue(babyQ,&babyQStart,&babyQCount);

                        score += 50;
                    }

                    return;
                }

                p = c;
                c = c->next;
            }
        }
    }
}
// ---------------- OBSTACLES ----------------
void InitObstacles(){
    for(int i=0;i<MAX_OBS;i++){
        obstacles[i].active=false;
        obstacles[i].moving=false;
        obstacles[i].dir=0;
        obstacles[i].blinkTimer=0;
    }
    numObstacles=0;
    if(level<2) return;
    float yShift = 40 + level*20;
    int wallCount = 3;
    if(level>=3) wallCount += (level-2);
    if(wallCount>MAX_OBS) wallCount = MAX_OBS;
    for(int i=0;i<wallCount;i++){
        float x,y;
        if(i==0){ x=30; y=60+yShift; }
        else if(i==1){ x=W-170; y=60+yShift; }
        else if(i==2){ x=W/2-70; y=110+yShift; }
        else{ x=60+(i*90)%(W-200); y=160+yShift+(i-3)*45; }
        obstacles[numObstacles].pos=(Vector2){x,y};
        obstacles[numObstacles].size=(Vector2){150,12}; // slightly bigger
        obstacles[numObstacles].active=true;
        obstacles[numObstacles].isSpike=false;
        obstacles[numObstacles].blinkTimer=0;
        obstacles[numObstacles].moving=(level>=3);
        obstacles[numObstacles].dir=(i%2==0)?1:-1;
        numObstacles++;
    }
}

void MoveObstacles(float dt){
    float baseSpeed = 20.0f;
    if(level>=4) baseSpeed += (level-3)*15.0f;
    for(int i=0;i<numObstacles;i++){
        if(!obstacles[i].active) continue;
        if(obstacles[i].moving){
            obstacles[i].pos.x += obstacles[i].dir*baseSpeed*dt;
            if(obstacles[i].pos.x<10){ obstacles[i].pos.x=10; obstacles[i].dir*=-1; }
            if(obstacles[i].pos.x+obstacles[i].size.x>W-10){ obstacles[i].pos.x=W-10-obstacles[i].size.x; obstacles[i].dir*=-1; }
        }
        if(obstacles[i].blinkTimer>0) obstacles[i].blinkTimer-=dt;
        Rectangle playerRec={player.x,player.y,P_SIZE,P_SIZE};
        Rectangle obsRec={obstacles[i].pos.x,obstacles[i].pos.y,obstacles[i].size.x,obstacles[i].size.y};
        if(CheckCollisionRecs(playerRec,obsRec) && !gameOver){
            gameOver=true;
            PlaySound(deadSound);
            UpdateHighScore(score);
        }
    }
}

void DrawObstacles(){
    Vector2 off = GetViewOffset();
    for(int i=0;i<numObstacles;i++){
        if(!obstacles[i].active) continue;
        Color c = obstacles[i].blinkTimer>0 ? YELLOW : GRAY;
        DrawRectangle(obstacles[i].pos.x, obstacles[i].pos.y, obstacles[i].size.x, obstacles[i].size.y, c);
    }
}

void ResetGame(){
    for(int i=0;i<MAX_Z;i++){
        zombies[i].active=false;
        zombies[i].entered=false;
        zombies[i].isBoss=false;
        zombies[i].isRedNormal=false;
        zombies[i].isRedBaby=false;
        zombies[i].isBaby=false;
    }

    while(bHead){
        Bullet* t=bHead;
        bHead=bHead->next;
        free(t);
    }

    qStart=qCount=0;
    babyQStart=babyQCount=0;

    score=0;
    level=1;
    bulletsFired=0;
    normalZombieKills=0;
    bossActive=false;
    zombieSpeed=1.2f;
    spawnInterval=2.0f;
    player=(Vector2){W/2,H-100};
    gameOver=false;
    gameWon = false;
}
// ---- CENTERING HELPERS (virtual resolution = W x H) ----

// ---------------- MAIN ----------------
int main(){

    if(sqlite3_open("zombiegame.db",&db)){
        printf("Failed to open DB\n");
        return 1;
    }

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS users("
        "id INTEGER PRIMARY KEY,"
        "username TEXT UNIQUE,"
        "password TEXT,"
        "highscore INTEGER DEFAULT 0);",
        0,0,0);

    // -------- WINDOW --------
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(W, H, "Zombie Game");
    MaximizeWindow();

    RenderTexture2D target = LoadRenderTexture(W, H);

    InitAudioDevice();
    SetTargetFPS(60);

    // -------- LOAD --------
    Texture2D bg = LoadTexture("background.png");
    Texture2D playerTex = LoadTexture("man.png");
    Texture2D zombieTex = LoadTexture("zombie.png");
    Texture2D redTex = LoadTexture("redzombie.png");
    Texture2D bossTex = LoadTexture("bigzombie.png");

    bulletSound = LoadSound("bullet.wav");
    deadSound = LoadSound("dead.wav");
    bigDeadSound = LoadSound("bigdead.wav");
    refillSound = LoadSound("refill.wav");

    // ================= LOGIN =================
    bool loginScreen = true;
    char username[50] = "";
    char password[50] = "";
    int focus = 0;
    bool showError = false;

    Rectangle userBox = {W/2-150, H/2-80, 300, 40};
    Rectangle passBox = {W/2-150, H/2-20, 300, 40};
    Rectangle loginBtn = {W/2-180, H/2+60, 160, 50};
    Rectangle regBtn = {W/2+20, H/2+60, 160, 50};

    while(loginScreen && !WindowShouldClose()){

        Vector2 mouse = GetMousePosition();

        float scale = fmin((float)GetScreenWidth()/W,
                           (float)GetScreenHeight()/H);

        float offsetX = (GetScreenWidth()-W*scale)/2;
        float offsetY = (GetScreenHeight()-H*scale)/2;

        mouse.x = (mouse.x - offsetX)/scale;
        mouse.y = (mouse.y - offsetY)/scale;

        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            if(CheckCollisionPointRec(mouse,userBox)) focus=0;
            else if(CheckCollisionPointRec(mouse,passBox)) focus=1;

            if(CheckCollisionPointRec(mouse, loginBtn)){
                if(LoginUser(username,password))
                    loginScreen=false;
                else showError=true;
            }

            if(CheckCollisionPointRec(mouse, regBtn)){
                if(RegisterUser(username,password)){
                    LoginUser(username,password);
                    loginScreen=false;
                }
                else showError=true;
            }
        }

        int key = GetCharPressed();
        while(key>0){
            if(key>=32 && key<127){
                if(focus==0 && strlen(username)<49){
                    int len=strlen(username);
                    username[len]=key;
                    username[len+1]=0;
                }
                if(focus==1 && strlen(password)<49){
                    int len=strlen(password);
                    password[len]=key;
                    password[len+1]=0;
                }
            }
            key = GetCharPressed();
        }

        if(IsKeyPressed(KEY_BACKSPACE)){
            if(focus==0 && strlen(username)>0)
                username[strlen(username)-1]=0;
            if(focus==1 && strlen(password)>0)
                password[strlen(password)-1]=0;
        }

        BeginTextureMode(target);
        ClearBackground(BLACK);

        DrawText("ZOMBIE HUNT", W/2-210, H/2-200, 70, YELLOW);
        DrawText("ZOMBIE HUNT (FIFO SURVIVAL)", W/2-240, H/2-135, 26, GRAY);
        DrawText("Username:", userBox.x - 120, userBox.y + 8, 22, WHITE);
        DrawText("Password:", passBox.x - 120, passBox.y + 8, 22, WHITE);
        DrawRectangleRec(userBox, focus==0?LIGHTGRAY:GRAY);
        DrawRectangleRec(passBox, focus==1?LIGHTGRAY:GRAY);

        DrawText(username, userBox.x+10, userBox.y+12, 20, DARKBLUE);
        DrawText(password, passBox.x+10, passBox.y+12, 20, DARKBLUE);

        DrawRectangleRec(loginBtn, BLUE);
        DrawText("Login", loginBtn.x+40, loginBtn.y+10, 30, WHITE);

        DrawRectangleRec(regBtn, DARKBLUE);
        DrawText("Register", regBtn.x+20, regBtn.y+10, 30, WHITE);

        if(showError)
            DrawText("Login/Register failed!", W/2-150, regBtn.y+70, 30, RED);

        EndTextureMode();

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(target.texture,
            (Rectangle){0,0,W,-H},
            (Rectangle){offsetX,offsetY,W*scale,H*scale},
            (Vector2){0,0},0,WHITE);
        EndDrawing();
    }

    // ================= GAME LOOP =================
    lastObstacleLevel = -1;

    while(!WindowShouldClose()){

        if(IsKeyPressed(KEY_M)){
            static bool muted=false;
            muted=!muted;
            SetMasterVolume(muted?0.0f:1.0f);
        }

        if(IsKeyPressed(KEY_P))
            paused = !paused;

        if(gameOver && IsKeyPressed(KEY_ZERO))
            ResetGame();

        // -------- BULLET LIMIT SYSTEM --------
        if(level==1) bulletLimit=-1;
        else if(level==2) bulletLimit=30;
        else if(level==3) bulletLimit=20;
        else if(level==4) bulletLimit=10;
        else if(level==5) bulletLimit=6;
        else if(level>=6) bulletLimit=2;

        if(IsKeyPressed(KEY_R) && level>=2){
            bulletsFired = 0;
            PlaySound(refillSound);
        }

        float dt = GetFrameTime();

        if(!paused && !gameOver){

            if(IsKeyDown(KEY_A)) player.x -= 4;
            if(IsKeyDown(KEY_D)) player.x += 4;
            if(IsKeyDown(KEY_W)) player.y -= 4;
            if(IsKeyDown(KEY_S)) player.y += 4;

            static bool shoot=false;
            if(IsKeyDown(KEY_SPACE) && !shoot){
                if(level==1 || (level>=2 && bulletsFired < bulletLimit)){
                    AddBullet((Vector2){player.x + P_SIZE/2, player.y});
                    shoot=true;
                    if(level>=2) bulletsFired++;
                }
            }
            if(!IsKeyDown(KEY_SPACE)) shoot=false;

            static float spawnT=0;
            spawnT+=dt;
            if(spawnT>spawnInterval){
                SpawnZombie(false,(Vector2){-1,-1});
                spawnT=0;
            }

            MoveBullets();
            BulletHitRed();
            MoveZombies();

            if(level!=lastObstacleLevel){
                InitObstacles();
                lastObstacleLevel=level;
            }

            MoveObstacles(dt);
        }


        // -------- DRAW --------
// -------- DRAW --------
BeginTextureMode(target);
ClearBackground(BLACK);

// --- Always draw world ---
if(!gameOver)
{
    DrawTexture(bg,0,0,WHITE);
}
else
{
    if(!gameWon)   // DEAD → show background
        DrawTexture(bg,0,0,WHITE);
    else           // WON → black screen
        ClearBackground(BLACK);
}

DrawTexturePro(playerTex,
    (Rectangle){0,0,(float)playerTex.width,(float)playerTex.height},
    (Rectangle){player.x,player.y,P_SIZE,P_SIZE},
    (Vector2){0,0},0,WHITE);

for(Bullet* b=bHead;b;b=b->next)
    DrawCircle((int)b->pos.x,(int)b->pos.y,BULLET_SIZE,YELLOW);

DrawZombies(zombieTex,redTex,bossTex);
DrawObstacles();

if(score > userHighScore)
    userHighScore = score;

if(bulletLimit<0){
    DrawText(TextFormat("Player: %s  Score: %d  High: %d  Level: %d  Bullets: ∞",
        loggedUsername, score, userHighScore, level),
        10,10,20,WHITE);
} else {
    int left = bulletLimit - bulletsFired;
    if(left<0) left=0;
    DrawText(TextFormat("Player: %s  Score: %d  High: %d  Level: %d  Bullets: %d/%d",
        loggedUsername, score, userHighScore, level, left, bulletLimit),
        10,10,20,WHITE);
}

DrawText("Press M to Mute/Unmute", W-280,H-30,20,GRAY);

// --- Overlay screens ---
if(gameOver)
{
    if(gameWon)
    {
        DrawText("CONGRATULATIONS!", W/2-240, H/2-40, 55, GREEN);
        DrawText("YOU SURVIVED ZOMBIES!", W/2-260, H/2+25, 32, WHITE);
        DrawText("Press 0 to Play Again", W/2-190, H/2+85, 25, GRAY);
    }
    else
    {
        DrawText("YOU'RE DEAD", W/2-180, H/2, 60, RED);
        DrawText("Press 0 to Restart", W/2-190, H/2+70, 30, WHITE);
    }
}
EndTextureMode();

        float scale = fmin((float)GetScreenWidth()/W,
                           (float)GetScreenHeight()/H);

        float offsetX = (GetScreenWidth()-W*scale)/2;
        float offsetY = (GetScreenHeight()-H*scale)/2;

        BeginDrawing();
        ClearBackground(BLACK);

        DrawTexturePro(
            target.texture,
            (Rectangle){0,0,W,-H},
            (Rectangle){offsetX,offsetY,W*scale,H*scale},
            (Vector2){0,0},0,WHITE);

        EndDrawing();
    }

    UnloadRenderTexture(target);
    UnloadTexture(bg);
    UnloadTexture(playerTex);
    UnloadTexture(zombieTex);
    UnloadTexture(redTex);
    UnloadTexture(bossTex);

    UnloadSound(bulletSound);
    UnloadSound(deadSound);
    UnloadSound(bigDeadSound);
    UnloadSound(refillSound);

    CloseAudioDevice();
    CloseWindow();
    sqlite3_close(db);

    return 0;
}