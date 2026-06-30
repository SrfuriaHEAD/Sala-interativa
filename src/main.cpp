#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <DNSServer.h>


#define GPIO_PIN   4
#define LED_PIN    48
#define BAUD       115200

const char* AP_SSID   = "Sala Interativa";
const char* AP_PASS   = "122REG3";
const char* ADMIN_PWD = "2REG3";

#define PATH_SESSAO  "/sessao.json"
#define PATH_SESSAO_TMP "/sessao.tmp"
#define PATH_SESSAO_BAK "/sessao.bak"
#define PATH_QUIZ    "/quiz.json"
#define PATH_LIVROS  "/livros.json"
#define DIR_HIST     "/hist"
#define MAX_HIST     20   // máximo de atividades arquivadas

// ── Escrita atômica: salva em .tmp → copia para .bak → move para destino final ──
// Nunca corrompe o arquivo principal mesmo se a energia cair no meio da escrita.
bool fsWrite(const char* path, const String& json) {
  // 1. Escreve em arquivo temporário
  String tmpPath = String(path) + ".tmp";
  File f = LittleFS.open(tmpPath.c_str(), "w");
  if (!f) return false;
  size_t written = f.print(json);
  f.close();
  if (written != json.length()) { LittleFS.remove(tmpPath.c_str()); return false; }

  // 2. Se existe arquivo atual, cria backup
  if (LittleFS.exists(path)) {
    String bakPath = String(path) + ".bak";
    LittleFS.remove(bakPath.c_str());
    LittleFS.rename(path, bakPath.c_str());
  }

  // 3. Promove o .tmp para o arquivo definitivo
  if (!LittleFS.rename(tmpPath.c_str(), path)) {
    LittleFS.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

String fsRead(const char* path) {
  // Tenta o arquivo principal; se falhar, tenta o backup
  File f = LittleFS.open(path, "r");
  if (!f) {
    String bakPath = String(path) + ".bak";
    f = LittleFS.open(bakPath.c_str(), "r");
    if (!f) return "";
    Serial.printf("[FS] AVISO: usando backup de %s\n", path);
  }
  String s = f.readString();
  f.close();
  return s;
}

bool fsEnsureDir(const char* dir) {
  if (!LittleFS.exists(dir)) return LittleFS.mkdir(dir);
  return true;
}

// ════════════════════════════════════════════════════════
//  QUIZ
// ════════════════════════════════════════════════════════
#define MAX_QUEST 8
struct Questao {
  char txt[120];
  char a[48], b[48], c[48], d[48];
  char gab;
  char obs[160];
};
Questao quiz[MAX_QUEST];
int  nQuiz      = 0;
bool gabLiberado = false;
bool provaAberta  = false;
bool provaFechada = false;

// ════════════════════════════════════════════════════════
//  VOTERS
// ════════════════════════════════════════════════════════
#define MAX_VOTERS 30
#define MAX_ADM    4

struct Voter {
  String ip, nome;
  char resp[MAX_QUEST];
  bool quizIniciado;
  bool presente;
  bool elegivel;          // ← NOVO: professor marcou para trocar de dispositivo
  char nomeNorm[32];      // ← NOVO: nome normalizado, chave de identidade
  unsigned long tInicioQ[MAX_QUEST];
  unsigned long tRespQ[MAX_QUEST];
};
Voter voters[MAX_VOTERS];
int nVoters = 0;

String admSess[MAX_ADM];
int nAdm = 0;

AsyncWebServer server(80);
DNSServer      dnsServer;


// ── Persistência periódica de sessão (evita LittleFS blocking por requisição) ──
volatile bool sessaoDirty = false;
unsigned long ultimoSalvamento = 0;
const unsigned long SALVAR_INTERVALO_MS = 10000UL;
inline void marcarSessaoDirty(){ sessaoDirty = true; }

// ════════════════════════════════════════════════════════
//  ÍNDICE DE LIVROS
// ════════════════════════════════════════════════════════
#define MAX_LIVROS 8
struct Livro {
  char titulo[48];
  char subtitulo[48];
  char prefixo[12];
  int  nPaginas;
  char cor[10];    // era 8 — #rrggbb\0 = 8, mas margem extra não custa
  char emoji[16];  // era 8 — emojis compostos chegam a 7-8 bytes, variantes até 14
};
Livro livros[MAX_LIVROS];
int nLivros = 0;

// ════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════
String normalizarNome(const String& s){
  String r = s; r.trim(); r.toLowerCase();
  String out = "";
  for(int i=0;i<(int)r.length();i++){
    char c = r[i];
    // remove acentos comuns
    if(c=='\xc3' && i+1<(int)r.length()){
      char n=r[i+1];
      if(n=='\xa0'||n=='\xa1'||n=='\xa2'||n=='\xa3'||n=='\xa4') { out+='a'; i++; continue; }
      if(n=='\xa8'||n=='\xa9'||n=='\xaa'||n=='\xab')             { out+='e'; i++; continue; }
      if(n=='\xac'||n=='\xad'||n=='\xae'||n=='\xaf')             { out+='i'; i++; continue; }
      if(n=='\xb2'||n=='\xb3'||n=='\xb4'||n=='\xb5'||n=='\xb6') { out+='o'; i++; continue; }
      if(n=='\xb9'||n=='\xba'||n=='\xbb'||n=='\xbc')             { out+='u'; i++; continue; }
      if(n=='\xa7') { out+='c'; i++; continue; } // ç
      if(n=='\x83') { out+='a'; i++; continue; } // ã maiúsculo via outro byte
    }
    if(c==' '){ if(out.length()>0 && out[out.length()-1]!=' ') out+=' '; continue; }
    if(c>='a'&&c<='z') out+=c;
    if(c>='0'&&c<='9') out+=c;
  }
  out.trim();
  return out;
}

Voter* getV(const String& ip){
  // 1. Procura pelo IP atual
  for(int i=0;i<nVoters;i++) if(voters[i].ip==ip) return &voters[i];
  return nullptr; // IP desconhecido — precisa registrar pelo nome
}

// Nova função: registra ou vincula pelo nome
Voter* registrarPorNome(const String& ip, const String& nome){
  String norm = normalizarNome(nome);
  if(norm.length() < 2) return nullptr;

  // 1. Já existe conta com esse nome normalizado?
  for(int i=0;i<nVoters;i++){
    if(String(voters[i].nomeNorm) == norm){
      if(voters[i].elegivel){
        // Transfere o IP para este dispositivo
        voters[i].ip = ip;
        voters[i].elegivel = false;
        marcarSessaoDirty();
        return &voters[i];
      } else {
        return nullptr; // nome em uso, não elegível
      }
    }
  }

  // 2. Nome novo — cria conta
  if(nVoters >= MAX_VOTERS) return nullptr;
  if(provaFechada) return nullptr;
  Voter v;
  v.ip = ip; v.nome = nome; v.quizIniciado = false;
  v.presente = false; v.elegivel = false;
  strncpy(v.nomeNorm, norm.c_str(), 31); v.nomeNorm[31] = 0;
  memset(v.resp, 0, MAX_QUEST);
  memset(v.tInicioQ, 0, sizeof(v.tInicioQ));
  memset(v.tRespQ, 0, sizeof(v.tRespQ));
  voters[nVoters] = v;
  return &voters[nVoters++];
}

bool isAdm(const String& ip){ for(int i=0;i<nAdm;i++) if(admSess[i]==ip) return true; return false; }
void addAdm(const String& ip){ if(!isAdm(ip)&&nAdm<MAX_ADM) admSess[nAdm++]=ip; }

String je(const char* s){
  String r="\"";
  for(int i=0;s[i];i++){
    if(s[i]=='"'||s[i]=='\\') r+='\\';
    else if(s[i]=='\n'){r+="\\n";continue;}
    else if(s[i]=='\r') continue;
    r+=s[i];
  }
  return r+'"';
}



// ════════════════════════════════════════════════════════
//  HISTÓRICO DE ATIVIDADES
// ════════════════════════════════════════════════════════
char nomeAtividade[48] = "Atividade";  // professor pode nomear antes de arquivar

// Retorna próximo ID de histórico (1..MAX_HIST, rotativo)
int proximoIdHist() {
  int maior = 0;
  File dir = LittleFS.open(DIR_HIST);
  if (!dir) return 1;
  File f = dir.openNextFile();
  while (f) {
    String nm = String(f.name());
    int id = nm.toInt();
    if (id > maior) maior = id;
    f = dir.openNextFile();
  }
  int proximo = maior + 1;
  if (proximo > MAX_HIST) proximo = 1;  // rotação
  return proximo;
}

void arquivarAtividade() {
  fsEnsureDir(DIR_HIST);
  int id = proximoIdHist();
  char path[32];
  snprintf(path, sizeof(path), "%s/%04d.json", DIR_HIST, id);

  unsigned long ts = millis() / 1000;  // segundos desde boot (sem RTC)

  String j = "{\"id\":" + String(id);
  j += ",\"nome\":" + String("\"") + String(nomeAtividade) + String("\"");
  j += ",\"ts\":" + String(ts);
  j += ",\"nQ\":" + String(nQuiz);
  j += ",\"gabLiberado\":" + String(gabLiberado ? "true" : "false");
  j += ",\"qs\":[";
  for (int i = 0; i < nQuiz; i++) {
    if (i) j += ",";
    char gs[2] = {quiz[i].gab, 0};
    j += "{\"txt\":" + String("\"") + String(quiz[i].txt) + String("\"");
    j += ",\"gab\":\"" + String(gs) + "\"}";
  }
  j += "],\"nAlunos\":" + String(nVoters);
  j += ",\"alunos\":[";
  for (int i = 0; i < nVoters; i++) {
    if (i) j += ",";
    j += "{\"nome\":" + String("\"") + (voters[i].nome.length() ? voters[i].nome : voters[i].ip) + String("\"");
    j += ",\"presente\":" + String(voters[i].presente ? "true" : "false");
    j += ",\"resp\":[";
    for (int k = 0; k < nQuiz; k++) {
      if (k) j += ",";
      char rs[2] = {voters[i].resp[k], 0};
      j += "\"" + String(voters[i].resp[k] ? rs : "") + "\"";
    }
    j += "],\"tempos\":[";
    for (int k = 0; k < nQuiz; k++) {
      if (k) j += ",";
      j += String(voters[i].tRespQ[k]);
    }
    j += "]}";
  }
  j += "]}";
  if (fsWrite(path, j))
    Serial.printf("[HIST] Arquivada atividade #%d em %s\n", id, path);
  else
    Serial.printf("[HIST] ERRO ao arquivar em %s\n", path);
}

// Lista IDs de histórico disponíveis, do mais recente ao mais antigo
String jsonListaHist() {
  String j = "{\"atividades\":[";
  File dir = LittleFS.open(DIR_HIST);
  if (!dir) { j += "]}"; return j; }
  // Coleta até MAX_HIST ids
  int ids[MAX_HIST]; int n = 0;
  File f = dir.openNextFile();
  while (f && n < MAX_HIST) {
    ids[n++] = String(f.name()).toInt();
    f = dir.openNextFile();
  }
  // Ordena decrescente (simples bubble sort, lista pequena)
  for (int a = 0; a < n - 1; a++)
    for (int b = a + 1; b < n; b++)
      if (ids[b] > ids[a]) { int t = ids[a]; ids[a] = ids[b]; ids[b] = t; }

  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    char path[32]; snprintf(path, sizeof(path), "%s/%04d.json", DIR_HIST, ids[i]);
    String raw = fsRead(path);
    if (raw.length() > 0) {
      // Extrai apenas cabeçalho: id, nome, ts, nQ, nAlunos
      // Faz parse manual mínimo para não consumir heap
      j += "{\"id\":" + String(ids[i]);
      // Extrai "nome":"..."
      int p1 = raw.indexOf("\"nome\":\"") + 8;
      int p2 = raw.indexOf("\"", p1);
      if (p1 > 7 && p2 > p1) j += ",\"nome\":\"" + raw.substring(p1, p2) + "\"";
      p1 = raw.indexOf("\"nQ\":") + 5; p2 = raw.indexOf(",", p1);
      if (p1 > 4) j += ",\"nQ\":" + raw.substring(p1, p2);
      p1 = raw.indexOf("\"nAlunos\":") + 10; p2 = raw.indexOf(",", p1); if(p2<0) p2=raw.indexOf("}", p1);
      if (p1 > 9) j += ",\"nAlunos\":" + raw.substring(p1, p2);
      j += "}";
    }
  }
  j += "]}";
  return j;
}

void resetQuiz(){
  for(int i=0;i<nVoters;i++){
    memset(voters[i].resp,0,MAX_QUEST);
    voters[i].quizIniciado=false;
    memset(voters[i].tInicioQ,0,sizeof(voters[i].tInicioQ));
    memset(voters[i].tRespQ,0,sizeof(voters[i].tRespQ));
  }
  gabLiberado=false;
  provaAberta=false;
  provaFechada=false;
}

// ════════════════════════════════════════════════════════
//  PERSISTÊNCIA
// ════════════════════════════════════════════════════════
void salvarQuiz(){
  String j="{\"nQuiz\":"+String(nQuiz)+",\"gabLiberado\":"+(gabLiberado?"true":"false")+",\"qs\":[";
  for(int i=0;i<nQuiz;i++){
    if(i) j+=",";
    char gs[2]={quiz[i].gab,0};
    j+="{\"txt\":"+je(quiz[i].txt)+",\"a\":"+je(quiz[i].a)+",\"b\":"+je(quiz[i].b);
    j+=",\"c\":"+je(quiz[i].c)+",\"d\":"+je(quiz[i].d);
    j+=",\"gab\":"+je(gs)+",\"obs\":"+je(quiz[i].obs)+"}";
  }
  j+="]}";
  fsWrite(PATH_QUIZ, j);
}

void carregarQuiz(){
  String raw=fsRead(PATH_QUIZ);
  if(raw.length()==0) return;
  StaticJsonDocument<8192> doc;
  if(deserializeJson(doc,raw)) return;
  nQuiz=doc["nQuiz"]|0;
  gabLiberado=doc["gabLiberado"]|false;
  JsonArray qs=doc["qs"]; int i=0;
  for(JsonObject q:qs){
    if(i>=MAX_QUEST) break;
    strncpy(quiz[i].txt,q["txt"]|"",119);
    strncpy(quiz[i].a,  q["a"]|"",47);
    strncpy(quiz[i].b,  q["b"]|"",47);
    strncpy(quiz[i].c,  q["c"]|"",47);
    strncpy(quiz[i].d,  q["d"]|"",47);
    strncpy(quiz[i].obs,q["obs"]|"",159);
    const char* g=q["gab"]|"";
    quiz[i].gab=(g[0]=='A'||g[0]=='B'||g[0]=='C'||g[0]=='D')?g[0]:0;
    i++;
  }
  nQuiz=i;
}

void salvarSessao(){
  String j="{\"provaAberta\":"+(String)(provaAberta?"true":"false");
  j+=",\"provaFechada\":"+(String)(provaFechada?"true":"false");
  j+=",\"nVoters\":"+String(nVoters)+",\"voters\":[";
  for(int i=0;i<nVoters;i++){
    if(i) j+=",";
    j+="{\"ip\":"+je(voters[i].ip.c_str());
    j+=",\"nome\":"+je(voters[i].nome.c_str());
    j+=",\"presente\":"+(String)(voters[i].presente?"true":"false");
    j+=",\"quizIniciado\":"+(String)(voters[i].quizIniciado?"true":"false");
    j += ",\"elegivel\":" + (String)(voters[i].elegivel ? "true" : "false");
    j += ",\"nomeNorm\":" + je(voters[i].nomeNorm);
     j+=",\"resp\":[";
     for(int k=0;k<MAX_QUEST;k++){if(k)j+=",";char rs[2]={voters[i].resp[k],0};j+=je(rs);}
     j+="],\"tResp\":[";
    for(int k=0;k<MAX_QUEST;k++){if(k)j+=",";j+=String(voters[i].tRespQ[k]);}
    j+="]}";
  }
  j+="]}";
  fsWrite(PATH_SESSAO, j);
}

void carregarSessao(){
  String raw=fsRead(PATH_SESSAO);
  if(raw.length()==0) return;
  size_t docSize=min((size_t)(raw.length()*2+512),(size_t)8192);
  DynamicJsonDocument doc(docSize);
  if(deserializeJson(doc,raw)) return;
  provaAberta =doc["provaAberta"]|false;
  provaFechada=doc["provaFechada"]|false;
  JsonArray vArr=doc["voters"]; nVoters=0;
  for(JsonObject vj:vArr){
    if(nVoters>=MAX_VOTERS) break;
    Voter& v=voters[nVoters];
    v.ip  =(const char*)(vj["ip"]|"");
    v.nome=(const char*)(vj["nome"]|"");
    v.presente=vj["presente"]|false;
    v.quizIniciado=vj["quizIniciado"]|false;
    v.elegivel = vj["elegivel"] | false;
    const char* nn = vj["nomeNorm"] | "";
    strncpy(v.nomeNorm, nn, 31); v.nomeNorm[31] = 0;
    memset(v.resp,0,MAX_QUEST);
    memset(v.tInicioQ,0,sizeof(v.tInicioQ));
    memset(v.tRespQ,0,sizeof(v.tRespQ));
    JsonArray rs=vj["resp"]; int k=0;
    for(JsonVariant r:rs){
      if(k>=MAX_QUEST) break;
      const char* s=r|"";
      v.resp[k]=(s[0]=='A'||s[0]=='B'||s[0]=='C'||s[0]=='D')?s[0]:0;
      k++;
    }
    k=0;
    JsonArray ts=vj["tResp"];
    for(JsonVariant t:ts){if(k>=MAX_QUEST)break;v.tRespQ[k]=t|0;k++;}
    nVoters++;
  }
}

void carregarLivros(){
  String raw=fsRead(PATH_LIVROS);
  if(raw.length()>0){
    StaticJsonDocument<2048> doc;
    if(!deserializeJson(doc,raw)){
      JsonArray arr=doc["livros"]; nLivros=0;
      for(JsonObject l:arr){
        if(nLivros>=MAX_LIVROS) break;
        strncpy(livros[nLivros].titulo,   l["titulo"]|"Livro",47);
        strncpy(livros[nLivros].subtitulo,l["subtitulo"]|"",47);
        strncpy(livros[nLivros].prefixo,  l["prefixo"]|"p",11);
        livros[nLivros].nPaginas=l["nPaginas"]|20;
        strncpy(livros[nLivros].cor,   l["cor"]  |"#1a4a8a", 9);  livros[nLivros].cor[9]   = '\0';
        strncpy(livros[nLivros].emoji, l["emoji"]|"📚",       15); livros[nLivros].emoji[15] = '\0';
        nLivros++;
      }
      if(nLivros>0) return;
    }
  }
  // Fallback
  strcpy(livros[0].titulo,"Matemática"); strcpy(livros[0].subtitulo,"Paiva Vol. II");
  strcpy(livros[0].prefixo,"p"); livros[0].nPaginas=21;
  strcpy(livros[0].cor,"#1a4a8a"); strcpy(livros[0].emoji,"📐");
  strcpy(livros[1].titulo,"Português"); strcpy(livros[1].subtitulo,"Gramática e Redação");
  strcpy(livros[1].prefixo,"pt"); livros[1].nPaginas=20;
  strcpy(livros[1].cor,"#4a1a2a"); strcpy(livros[1].emoji,"📖");
  strcpy(livros[2].titulo,"Redação"); strcpy(livros[2].subtitulo,"Técnicas e Modelos");
  strcpy(livros[2].prefixo,"rd"); livros[2].nPaginas=15;
  strcpy(livros[2].cor,"#1a4a2a"); strcpy(livros[2].emoji,"✍️");
  nLivros=3;
}

void carregarQuestoesPadrao(){
  provaAberta=false; provaFechada=false;

  strncpy(quiz[0].txt,"O numero mensal de passagens de certa empresa aerea aumentou com as seguintes condicoes: jan=33.000, fev=34.500, mar=36.000. Esse padrao se mantem. Quantas passagens foram vendidas em julho?",119);
  strncpy(quiz[0].a,"38.000",47); strncpy(quiz[0].b,"40.500",47);
  strncpy(quiz[0].c,"41.000",47); strncpy(quiz[0].d,"42.000",47); quiz[0].gab='D';
  strncpy(quiz[0].obs,"PA com razao 1500. a7 = 33000 + 6x1500 = 42.000.",159);

  strncpy(quiz[1].txt,"Numa PA, o primeiro termo e 5 e a razao e 3. Qual e o 10 termo?",119);
  strncpy(quiz[1].a,"28",47); strncpy(quiz[1].b,"30",47);
  strncpy(quiz[1].c,"32",47); strncpy(quiz[1].d,"35",47); quiz[1].gab='C';
  strncpy(quiz[1].obs,"an = a1 + (n-1)*r -> a10 = 5 + 9x3 = 32.",159);

  strncpy(quiz[2].txt,"Os 5 primeiros termos de uma PA sao 2, 5, 8, 11, 14. Qual e a soma desses termos?",119);
  strncpy(quiz[2].a,"35",47); strncpy(quiz[2].b,"38",47);
  strncpy(quiz[2].c,"40",47); strncpy(quiz[2].d,"42",47); quiz[2].gab='C';
  strncpy(quiz[2].obs,"Sn = n*(a1+an)/2 = 5*(2+14)/2 = 40.",159);

  nQuiz=3;
}

// ════════════════════════════════════════════════════════
//  CSS COMPARTILHADO
// ════════════════════════════════════════════════════════
const char CSS[] PROGMEM = R"css(
:root{
  --bg:#0b0c10;--s:#12131a;--b:#1e2030;--b2:#252840;
  --on:#00ffa3;--off:#ff3e5e;--ac:#ff6b35;--ac2:#ffb347;
  --blue:#4a9eff;--tx:#dde2f0;--mu:#5a6080;--mu2:#3a4060;--yw:#ffd166
}
*{box-sizing:border-box;margin:0;padding:0}
html{scroll-behavior:smooth}
body{font-family:'IBM Plex Mono',monospace;background:var(--bg);color:var(--tx);min-height:100vh;padding-bottom:5rem;-webkit-font-smoothing:antialiased}
body::before{content:'';position:fixed;inset:0;pointer-events:none;z-index:0;background:radial-gradient(ellipse 60% 35% at 80% 5%,#ff6b3508,transparent),radial-gradient(ellipse 50% 30% at 10% 90%,#4a9eff06,transparent)}
.wrap{position:relative;z-index:1;max-width:480px;margin:0 auto;padding:0 1rem}
.school-bar{text-align:center;padding:.6rem 0 0;position:relative;z-index:1}
.school-name{font-family:'DM Serif Display',serif;font-size:1.1rem;font-weight:400;color:#4a9eff;letter-spacing:.02em}
.school-dev{font-size:.48rem;color:var(--mu2);letter-spacing:.18em;text-transform:uppercase;margin-top:.12rem;font-weight:600}
.hd{text-align:center;padding:1.2rem 0 1rem}
.hd h1{font-family:'DM Serif Display',serif;font-size:1.8rem;font-weight:400;letter-spacing:.01em;color:var(--tx);line-height:1.1}
.hd h1 em{font-style:italic;color:var(--ac)}
.hd p{font-size:.58rem;color:var(--mu);letter-spacing:.2em;text-transform:uppercase;margin-top:.35rem}
.card{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1.3rem;margin-bottom:.85rem;position:relative;overflow:hidden}
.card::after{content:'';position:absolute;top:0;left:1.3rem;right:1.3rem;height:1px;background:linear-gradient(90deg,transparent,var(--b2),transparent)}
.sect{font-family:'IBM Plex Mono',monospace;font-size:.62rem;font-weight:600;letter-spacing:.14em;text-transform:uppercase;color:var(--ac);margin-bottom:.9rem;padding-bottom:.5rem;border-bottom:1px solid var(--b)}
.row{display:flex;justify-content:space-between;align-items:center;padding:.42rem 0;border-bottom:1px solid var(--b);font-size:.7rem}
.row:last-child{border-bottom:none}
.lbl{color:var(--mu);letter-spacing:.05em}
.val{font-weight:500}
.val.on{color:var(--on)}.val.off{color:var(--off)}.val.ac{color:var(--ac)}.val.blue{color:var(--blue)}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:.65rem}
.btn{cursor:pointer;border:none;border-radius:9px;padding:.85rem .4rem;font-family:'IBM Plex Mono',monospace;font-size:.78rem;font-weight:600;letter-spacing:.06em;text-transform:uppercase;transition:transform .1s,filter .2s,box-shadow .2s}
.btn:active{transform:scale(.95)}
.btn:disabled{opacity:.28;cursor:not-allowed;transform:none;filter:none}
.btn-on{background:var(--on);color:#001a0d;box-shadow:0 4px 18px #00ffa322}
.btn-on:hover:not(:disabled){filter:brightness(1.1)}
.btn-off{background:var(--off);color:#fff;box-shadow:0 4px 18px #ff3e5e22}
.btn-ac{background:var(--ac);color:#1a0a00;box-shadow:0 4px 18px #ff6b3522}
.btn-ac:hover:not(:disabled){filter:brightness(1.1)}
.btn-neu{background:var(--b2);color:var(--tx);border:1px solid var(--b)}
.btn-back{background:transparent;border:1px solid var(--b);color:var(--mu);font-size:.65rem;padding:.6rem;width:100%;margin-top:.3rem;border-radius:9px;cursor:pointer;font-family:'IBM Plex Mono',monospace;letter-spacing:.08em;text-transform:uppercase;transition:all .2s}
.btn-back:hover{border-color:var(--mu);color:var(--tx)}
.btn-sm{font-size:.62rem;padding:.46rem .4rem;border-radius:7px}
.btn-verde{background:#001a0d;color:var(--on);border:1px solid #00ffa333;font-size:.65rem}
.btn-danger{background:#1a0408;color:var(--off);border:1px solid #ff3e5e33;font-size:.65rem}
.btn-warn{background:#1a1200;color:var(--yw);border:1px solid #ffd16633;font-size:.65rem}
.btn-blue{background:#001020;color:var(--blue);border:1px solid #4a9eff33;font-size:.65rem}
.adm-btn{position:fixed;top:.8rem;right:.8rem;z-index:50;background:var(--s);border:1px solid var(--b2);color:var(--mu);border-radius:7px;padding:.38rem .55rem;font-size:.85rem;cursor:pointer;transition:all .2s;line-height:1}
.adm-btn:hover{border-color:var(--ac);color:var(--ac)}
.ov{display:none;position:fixed;inset:0;background:rgba(0,0,0,.88);backdrop-filter:blur(10px);z-index:100;align-items:center;justify-content:center;padding:1rem}
.ov.open{display:flex}
.modal{background:var(--s);border:1px solid var(--b2);border-radius:18px;padding:1.8rem;width:100%;max-width:310px}
.modal h2{font-family:'DM Serif Display',serif;font-size:1.4rem;font-weight:400;margin-bottom:.9rem;color:var(--tx)}
.inp{width:100%;background:var(--bg);border:1px solid var(--b2);border-radius:8px;padding:.62rem .82rem;color:var(--tx);font-family:'IBM Plex Mono',monospace;font-size:.78rem;outline:none;margin-bottom:.58rem;transition:border-color .2s}
.inp:focus{border-color:var(--ac)}
.merr{color:var(--off);font-size:.6rem;letter-spacing:.07em;min-height:.8em;margin-bottom:.45rem}
.mbtns{display:flex;gap:.5rem}
.mok{flex:1;background:var(--ac);color:#1a0a00;border:none;border-radius:8px;padding:.7rem;font-family:'IBM Plex Mono',monospace;font-size:.82rem;font-weight:600;letter-spacing:.07em;cursor:pointer;text-transform:uppercase}
.mno{background:var(--b2);color:var(--mu);border:none;border-radius:8px;padding:.7rem .8rem;font-size:.7rem;cursor:pointer;font-family:'IBM Plex Mono',monospace}
#toast{position:fixed;bottom:1.4rem;left:50%;transform:translateX(-50%) translateY(50px);background:var(--s);border:1px solid var(--b2);color:var(--tx);padding:.5rem 1.2rem;border-radius:999px;font-size:.62rem;letter-spacing:.07em;transition:transform .3s,opacity .3s;opacity:0;pointer-events:none;z-index:999;white-space:nowrap}
#toast.show{transform:translateX(-50%) translateY(0);opacity:1}
.nome-chip{display:inline-flex;align-items:center;gap:.38rem;background:var(--b);border:1px solid var(--b2);border-radius:999px;padding:.2rem .7rem;font-size:.58rem;color:var(--ac);letter-spacing:.06em;cursor:pointer;transition:border-color .2s}
.nome-chip:hover{border-color:var(--ac)}
footer{text-align:center;font-size:.52rem;color:var(--mu2);letter-spacing:.1em;text-transform:uppercase;padding-top:.5rem}
)css";

// ════════════════════════════════════════════════════════
//  PÁGINA: INDEX
// ════════════════════════════════════════════════════════
const char PG_INDEX[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sala Interativa</title><style>%CSS%
.hero{padding:1rem 0 .6rem;text-align:center}
.hero h1{font-family:'DM Serif Display',serif;font-size:2.2rem;font-weight:400;line-height:1.1;color:var(--tx)}
.hero h1 em{font-style:italic;color:var(--ac)}
.hero p{font-size:.58rem;color:var(--mu);letter-spacing:.2em;text-transform:uppercase;margin-top:.4rem}
.nav-grid{display:flex;flex-direction:column;gap:.58rem;margin-top:.7rem}
.nav-card{display:flex;align-items:center;gap:.9rem;background:var(--s);border:1px solid var(--b);border-radius:13px;padding:1rem 1.1rem;text-decoration:none;color:var(--tx);transition:border-color .2s,transform .15s,box-shadow .2s;position:relative;overflow:hidden}
.nav-card:hover{border-color:var(--b2);transform:translateX(3px);box-shadow:0 4px 20px rgba(0,0,0,.3)}
.nav-card::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px}
.nav-card.c-blue::before{background:var(--blue)}
.nav-card.c-yw::before{background:var(--yw)}
.nav-ico{font-size:1.6rem;min-width:1.8rem;text-align:center}
.nav-txt h3{font-family:'DM Serif Display',serif;font-size:.95rem;font-weight:400;letter-spacing:.01em}
.nav-txt p{font-size:.57rem;color:var(--mu);letter-spacing:.04em;margin-top:.12rem}
.nav-badge{margin-left:auto;font-family:'IBM Plex Mono',monospace;font-size:.58rem;padding:.18rem .5rem;border-radius:5px;border:1px solid;white-space:nowrap;flex-shrink:0;letter-spacing:.05em;color:var(--mu);border-color:var(--b2);background:var(--b)}
.nav-badge.amarelo{color:var(--yw);border-color:#ffd16622;background:#ffd16608}
.nome-bar{display:flex;justify-content:center;margin-bottom:.4rem}
</style></head><body>
<button class="adm-btn" onclick="abrirAdmin()" title="Admin">⚙</button>
<div class="ov" id="ov">
  <div class="modal">
    <h2>Admin</h2>
    <input class="inp" type="password" id="pwd" placeholder="senha" maxlength="20" onkeydown="if(event.key==='Enter')tentarAdmin()">
    <p class="merr" id="merr"></p>
    <div class="mbtns">
      <button class="mok" onclick="tentarAdmin()">ENTRAR</button>
      <button class="mno" onclick="fecharAdmin()">✕</button>
    </div>
  </div>
</div>
<div class="ov" id="ovNome">
  <div class="modal">
    <h2>Seu nome</h2>
    <p style="font-size:.63rem;color:var(--mu);margin-bottom:.8rem;line-height:1.5">Para aparecer no painel da sala</p>
    <input class="inp" type="text" id="nomeInp" placeholder="Ex: João Silva" maxlength="24" onkeydown="if(event.key==='Enter')salvarNome()">
    <div class="mbtns"><button class="mok" onclick="salvarNome()">PRONTO</button></div>
  </div>
</div>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="wrap">
  <div class="hero">
    <h1><em>Sala</em> Interativa</h1>
    <p id="saudacao">ESP32 · 192.168.4.1</p>
  </div>
  <div class="nome-bar">
    <span class="nome-chip" id="nomeChip" onclick="editarNome()">
      ◎ <span id="nomeLabel">—</span> ✎
    </span>
  </div>
  <div class="nav-grid">
    <a class="nav-card c-blue" href="/atividades">
      <span class="nav-ico">🧠</span>
      <div class="nav-txt"><h3>Atividades</h3><p>Questões de matemática</p></div>
      <span class="nav-badge" id="badgeAtiv">—</span>
    </a>
    <a class="nav-card c-yw" href="/livros">
      <span class="nav-ico">📚</span>
      <div class="nav-txt"><h3>Biblioteca</h3><p>Livros digitais da turma</p></div>
      <span class="nav-badge amarelo" id="badgeLivros">—</span>
    </a>
  </div>
  <footer style="margin-top:1rem">Sala Interativa · ESP32 AP</footer>
</div>
<div id="toast"></div>
<script>
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.classList.add('show');setTimeout(()=>e.classList.remove('show'),2200)}
var nome=localStorage.getItem('sala_nome')||'';
function atualizarNome(){
  document.getElementById('nomeLabel').textContent=nome||'Sem nome';
  document.getElementById('saudacao').textContent=nome?'Olá, '+nome+' 👋':'Toque para se identificar';
  if(nome)fetch('/api/nome?n='+encodeURIComponent(nome));
}
function salvarNome(){
  var v=document.getElementById('nomeInp').value.trim();if(!v)return;
  nome=v;localStorage.setItem('sala_nome',nome);
  document.getElementById('ovNome').classList.remove('open');
  atualizarNome();toast('Nome salvo ✓');
}
function editarNome(){
  document.getElementById('nomeInp').value=nome;
  document.getElementById('ovNome').classList.add('open');
  setTimeout(()=>document.getElementById('nomeInp').focus(),80);
}
function abrirAdmin(){document.getElementById('ov').classList.add('open');document.getElementById('pwd').value='';document.getElementById('merr').textContent='';setTimeout(()=>document.getElementById('pwd').focus(),80)}
function fecharAdmin(){document.getElementById('ov').classList.remove('open')}
document.getElementById('ov').addEventListener('click',function(e){if(e.target===this)fecharAdmin()})
function tentarAdmin(){
  fetch('/admin/auth?senha='+encodeURIComponent(document.getElementById('pwd').value))
    .then(r=>r.json()).then(d=>{if(d.ok)location.href='/admin/painel';else document.getElementById('merr').textContent='Senha incorreta.'}).catch(()=>document.getElementById('merr').textContent='Erro.')
}
function atualizar(){
  fetch('/api/idx').then(r=>r.json()).then(d=>{
    document.getElementById('badgeAtiv').textContent=d.nQ+' questão'+(d.nQ!==1?'ões':'');
    document.getElementById('badgeLivros').textContent=d.nLivros+' livro'+(d.nLivros!==1?'s':'');
  }).catch(()=>{})
}
atualizarNome();
if(!nome){setTimeout(()=>{document.getElementById('ovNome').classList.add('open');setTimeout(()=>document.getElementById('nomeInp').focus(),80);},500);}
atualizar();setInterval(atualizar,5000)
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  PÁGINA: ATIVIDADES
// ════════════════════════════════════════════════════════
const char PG_ATIV[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Atividades · Sala</title>
<style>%CSS%
.qcard{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1.3rem;margin-bottom:.85rem}
.qnum{font-size:.54rem;color:var(--ac);letter-spacing:.16em;text-transform:uppercase;margin-bottom:.5rem}
.qtxt{font-size:.82rem;line-height:1.65;margin-bottom:1rem;color:var(--tx)}
.prova-header{background:var(--s);border:1px solid var(--b);border-radius:12px;padding:.85rem 1.1rem;margin-bottom:.75rem;display:flex;align-items:center;justify-content:space-between}
.prova-progresso{font-family:'DM Serif Display',serif;font-size:.9rem;color:var(--ac)}
.prova-barra{height:2px;background:var(--b);border-radius:1px;overflow:hidden;margin-bottom:.75rem}
.prova-barra-fi{height:100%;background:linear-gradient(90deg,var(--ac),var(--ac2));border-radius:1px;transition:width .5s}
.abcd{display:flex;flex-direction:column;gap:.45rem}
.ab{cursor:pointer;background:var(--b);border:1px solid var(--b2);border-radius:10px;padding:.72rem .9rem;font-size:.7rem;display:flex;gap:.6rem;align-items:flex-start;transition:all .18s;line-height:1.45}
.ab:active:not(.dis){transform:scale(.98)}
.ab:hover:not(.dis){border-color:var(--blue);background:#4a9eff08}
.ab.dis{cursor:default;pointer-events:none}
.al{font-family:'DM Serif Display',serif;font-size:.95rem;min-width:1rem;flex-shrink:0;margin-top:.05rem;color:var(--mu)}
.ab:hover:not(.dis) .al{color:var(--blue)}
.ab.gab-correto{border-color:var(--on)!important;background:#00ffa30a!important}
.ab.gab-correto .al{color:var(--on)!important}
.ab.gab-errou{border-color:var(--off)!important;background:#ff3e5e0a!important}
.ab.gab-errou .al{color:var(--off)!important}
.ab.gab-sel-correto{border-color:var(--on)!important;background:#00ffa315!important}
.ab.gab-sel-correto .al{color:var(--on)!important}
.obs-box{background:var(--b);border:1px solid var(--b2);border-left:3px solid var(--ac);border-radius:8px;padding:.7rem .9rem;margin-top:.75rem;font-size:.65rem;color:var(--ac2);line-height:1.6}
.obs-label{font-size:.52rem;color:var(--ac);letter-spacing:.12em;text-transform:uppercase;margin-bottom:.25rem}
.resultado{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1.5rem;text-align:center}
.resultado-score{font-family:'DM Serif Display',serif;font-size:3rem;font-style:italic;color:var(--ac);margin:.7rem 0}
.resumo-item{display:flex;justify-content:space-between;font-size:.63rem;padding:.28rem 0;border-bottom:1px solid var(--b)}
.resumo-item:last-child{border-bottom:none}
.resumo-item .ok{color:var(--on)}.resumo-item .err{color:var(--off)}.resumo-item .pend{color:var(--mu)}
.vazio{text-align:center;font-size:.7rem;color:var(--mu);padding:1.4rem;letter-spacing:.07em}
.load-msg{text-align:center;padding:2rem;font-size:.62rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase}
.entrada-card{background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1.3rem;margin-bottom:.85rem}
.entrada-titulo{font-family:'DM Serif Display',serif;font-size:1.2rem;margin-bottom:.5rem}
.entrada-sub{font-size:.62rem;color:var(--mu);letter-spacing:.05em;margin-bottom:1rem;line-height:1.5}
.presenca-lista{margin-top:1rem;font-size:.6rem;color:var(--mu);letter-spacing:.05em}
.presenca-item{padding:.22rem 0;border-bottom:1px solid var(--b)}
.presenca-item:last-child{border-bottom:none}
.presenca-item.eu{color:var(--on)}
</style></head><body>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="wrap">
  <div class="hd"><h1><em>Atividades</em></h1><p id="hdSub">Matemática</p></div>
  <div id="area"><div class="load-msg">Carregando...</div></div>
  <button class="btn btn-back" onclick="location.href='/'">← Início</button>
</div>
<div id="toast"></div>
<script>
var nome=localStorage.getItem('sala_nome')||'';
if(nome)fetch('/api/nome?n='+encodeURIComponent(nome));
var respondendo=false;
var confirmando=false;   // ← linha nova
var LETRAS=['A','B','C','D'];
var tInicioAtual=0; // millis local do início da questão atual

function esc(s){if(!s)return'';var d=document.createElement('div');d.appendChild(document.createTextNode(s));return d.innerHTML}
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.classList.add('show');setTimeout(function(){e.classList.remove('show');},2200);}

function renderListaPresenca(d){
  var presentes=(d.presentes||[]);
  var jaEntrou = d.euPresente === true;
  var h='<div class="entrada-card">';
  h+='<div class="entrada-titulo">Lista de presença</div>';
  h+='<div class="entrada-sub">';
  if(!d.provaAberta) h+='O professor ainda não abriu a atividade. Confirme sua presença para aguardar.';
  else h+='Atividade disponível! Confirme sua presença para começar.';
  h+='</div>';
  if(!jaEntrou){
    h+='<button class="btn btn-ac" style="width:100%" onclick="confirmarPresenca()">Confirmar presença</button>';
  } else {
    h+='<p style="text-align:center;font-size:.62rem;color:var(--on);letter-spacing:.07em">✓ Presença confirmada</p>';
    if(d.provaAberta){
      h+='<button onclick="iniciarProva()" style="display:block;width:100%;margin-top:.6rem;background:var(--blue);color:#001020;border:none;border-radius:9px;padding:.85rem;font-family:\'IBM Plex Mono\',monospace;font-size:.78rem;font-weight:600;letter-spacing:.06em;text-transform:uppercase;cursor:pointer">Entrar na atividade →</button>';
    }
  }
  if(presentes.length>0){
    h+='<div class="presenca-lista"><div style="font-size:.52rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.3rem">Na sala ('+presentes.length+')</div>';
    presentes.forEach(function(n){h+='<div class="presenca-item'+(n===nome?' eu':'')+'">'+esc(n)+(n===nome?' ← você':'')+'</div>';});
    h+='</div>';
  }
  h+='</div>';
  document.getElementById('area').innerHTML=h;
}

function renderTela(d){
  var a=document.getElementById('area');
  if(!d.qs||!d.qs.length){a.innerHTML='<p class="vazio">Nenhuma atividade disponível.</p>';return;}
  var nQ=d.qs.length, rs=d.rs||[];
  var todas=rs.filter(function(r){return r !== "" && r !== "\0" && r;}).length===nQ;
  if(todas){renderResultado(d);return;}
  var idx=-1;
  for(var i=0;i<nQ;i++){if(!rs[i]||rs[i]==='\0'){idx=i;break;}}
  if(idx===-1){renderResultado(d);return;}
  var q=d.qs[idx];
  var respondidas=rs.filter(function(r){return r !== "" && r !== "\0" && r;}).length;
  var pct=Math.round(respondidas/nQ*100);
  tInicioAtual=Date.now(); // marca início local da questão
  var h='';
  h+='<div class="prova-header">';
  h+='<span class="prova-progresso">'+(idx+1)+' / '+nQ+'</span>';
  h+='<span style="font-size:.58rem;color:var(--mu)">'+respondidas+' respondida'+(respondidas!==1?'s':'')+'</span>';
  h+='</div>';
  h+='<div class="prova-barra"><div class="prova-barra-fi" style="width:'+pct+'%"></div></div>';
  h+='<div class="qcard"><div class="qnum">Questão '+(idx+1)+'</div>';
  h+='<div class="qtxt">'+esc(q.t)+'</div>';
  h+='<div class="abcd">';
  for(var li=0;li<q.opts.length;li++){
    if(!q.opts[li])continue;
    var letra=LETRAS[li];
    var cls='ab';
    if(d.gabLiberado&&rs[idx]){
      var estaCorreta=q.gab&&letra===q.gab;
      var alunoEscolheu=rs[idx]===letra;
      if(estaCorreta&&alunoEscolheu)cls+=' gab-sel-correto';
      else if(estaCorreta)cls+=' gab-correto';
      else if(alunoEscolheu&&!estaCorreta)cls+=' gab-errou';
    }
    var onclick=(!rs[idx]&&!respondendo)?'onclick="responder('+idx+',\''+letra+'\')"':'';
    h+='<div class="'+cls+'"'+onclick+'><span class="al">'+letra+'</span>'+esc(q.opts[li])+'</div>';
  }
  h+='</div>';
  if(d.gabLiberado&&rs[idx]&&q.obs&&q.obs.trim()){
    h+='<div class="obs-box"><div class="obs-label">Por que?</div>'+esc(q.obs)+'</div>';
  }
  h+='</div>';
  a.innerHTML=h;
}

function renderResultado(d){
  var a=document.getElementById('area');
  var nQ=d.qs?d.qs.length:0, rs=d.rs||[];
  var respondidas=rs.filter(function(r){return r !== "" && r !== "\0" && r;}).length;
  var acertos=0;
  var temGab=d.gabLiberado&&d.qs&&d.qs.some(function(q){return q.gab&&q.gab!=='0';});
  if(temGab){for(var i=0;i<nQ;i++){if(rs[i]&&d.qs[i].gab&&rs[i]===d.qs[i].gab)acertos++;}}
  var h='<div class="resultado">';
  h+='<div style="font-size:2.5rem;margin-bottom:.5rem">'+(temGab?(acertos>=nQ*.7?'🏆':acertos>=nQ*.4?'📊':'📝'):'✅')+'</div>';
  h+='<div style="font-family:\'DM Serif Display\',serif;font-size:1.1rem;color:var(--tx)">'+(temGab?(acertos>=nQ*.7?'Muito bem!':'Prova concluída'):'Atividade concluída!')+'</div>';
  h+='<div class="resultado-score">'+(temGab?acertos+'/'+nQ:respondidas+'/'+nQ)+'</div>';
  h+='<p style="font-size:.6rem;color:var(--mu);letter-spacing:.06em">'+(temGab?(acertos+' acerto'+(acertos!==1?'s':'')+' de '+nQ):'Aguarde o gabarito do professor.')+'</p>';
  h+='<div style="margin-top:1rem;text-align:left">';
  for(var i=0;i<nQ;i++){
    if(!d.qs[i])continue;
    var r=rs[i],g=d.gabLiberado?d.qs[i].gab:null;
    var ok=(g&&g!=='0'&&g!=='\0')?(r===g):null;
    var cls2=ok===null?'pend':(ok?'ok':'err'),ico2=ok===null?'◦':(ok?'✓':'✗');
    var gabInfo='';
    if(ok===false&&g&&g!=='0') gabInfo=' <span style="color:var(--on);font-size:.55rem">→ Certa: '+g+'</span>';
    h+='<div class="resumo-item"><span>Q'+(i+1)+'</span><span class="'+cls2+'">'+ico2+' '+(r||'—')+gabInfo+'</span></div>';
    if(d.gabLiberado&&r&&d.qs[i].obs&&d.qs[i].obs.trim()){
      h+='<div class="obs-box" style="margin-bottom:.5rem"><div class="obs-label">Q'+(i+1)+' — Por que?</div>'+esc(d.qs[i].obs)+'</div>';
    }
  }
  h+='</div></div>';
  a.innerHTML=h;
}

function renderEstado(d){
  if(!d.provaAberta){
    document.getElementById('area').innerHTML='<div style="text-align:center;padding:2rem"><div style="font-size:2.8rem;margin-bottom:.8rem">⏳</div><div style="font-family:\'DM Serif Display\',serif;font-size:1.3rem;color:var(--tx);margin-bottom:.4rem">Aguardando professor</div><div style="font-size:.62rem;color:var(--mu);letter-spacing:.06em">A atividade ainda não foi aberta.</div></div>';
    return;
  }
    // DEPOIS
if(d.provaFechada&&!d.quizIniciado&&!d.euPresente){
    document.getElementById('area').innerHTML='<div style="text-align:center;padding:2rem"><div style="font-size:2rem;margin-bottom:.6rem">🔒</div><div style="font-family:\'DM Serif Display\',serif;font-size:1.3rem;color:var(--tx);margin-bottom:.4rem">Atividade encerrada</div><div style="font-size:.62rem;color:var(--mu)">O professor encerrou as inscrições.</div></div>';
    return;
  }
  if(!d.quizIniciado){renderListaPresenca(d);return;}
  renderTela(d);
}

function confirmarPresenca(){
  if(confirmando)return;
  confirmando=true;
  fetch('/api/quiz/evento?op=presente'+(nome?'&n='+encodeURIComponent(nome):'')).then(r=>r.json()).then(function(d){confirmando=false;renderEstado(d);}).catch(function(){confirmando=false;toast('Erro.');});
}
function iniciarProva(){
  fetch('/api/quiz/evento?op=init'+(nome?'&n='+encodeURIComponent(nome):'')).then(r=>r.json()).then(renderEstado).catch(()=>toast('Erro.'));
}

function responder(idx, letra){
  if(respondendo)return;
  respondendo=true;
  var tempoMs=tInicioAtual>0?Math.round(Date.now()-tInicioAtual):0;
  var url='/api/quiz?q='+idx+'&r='+encodeURIComponent(letra)+'&ms='+tempoMs+(nome?'&n='+encodeURIComponent(nome):'');
  fetch(url).then(function(r){return r.json();}).then(function(d){
    if(d.erro){toast(d.erro);respondendo=false;return;}
    toast('Resposta registrada ✓');
    setTimeout(function(){respondendo=false;renderEstado(d);},600);
  }).catch(function(){toast('Erro.');respondendo=false;});
}

var pollErros = 0;
function poll(){
  fetch('/api/quiz' + (nome ? '?n=' + encodeURIComponent(nome) : ''))
    .then(function(r){ return r.json(); })
    .then(function(d){ pollErros=0; renderEstado(d); })
    .catch(function(){ 
      pollErros++;
      if(pollErros >= 3) toast('Sem conexão com a sala...');
    });
}
poll();
setInterval(poll, 4000);
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  PÁGINA: BIBLIOTECA
// ════════════════════════════════════════════════════════
const char PG_BIBLIOTECA[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Biblioteca · Sala</title><style>%CSS%
.livros-grid{display:flex;flex-direction:column;gap:.65rem;margin-top:.3rem}
.livro-card{display:flex;align-items:center;gap:.9rem;background:var(--s);border:1px solid var(--b);border-radius:13px;padding:1rem 1.1rem;text-decoration:none;color:var(--tx);transition:border-color .2s,transform .15s;position:relative;overflow:hidden}
.livro-card::before{content:'';position:absolute;left:0;top:0;bottom:0;width:3px;background:var(--cor-livro,var(--ac))}
.livro-card:hover{border-color:var(--b2);transform:translateX(3px)}
.livro-ico{font-size:2rem;min-width:2.2rem;text-align:center}
.livro-info h3{font-family:'DM Serif Display',serif;font-size:.95rem;font-weight:400}
.livro-info p{font-size:.56rem;color:var(--mu);letter-spacing:.04em;margin-top:.1rem}
.livro-pgs{margin-left:auto;font-size:.56rem;color:var(--mu);border:1px solid var(--b2);border-radius:5px;padding:.15rem .45rem;white-space:nowrap;flex-shrink:0}
.sem-livros{text-align:center;padding:2rem;font-size:.68rem;color:var(--mu);letter-spacing:.07em}
</style></head><body>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="wrap">
  <div class="hd"><h1><em>Biblioteca</em></h1><p>Livros digitais da turma</p></div>
  <div class="livros-grid" id="lista"></div>
  <button class="btn btn-back" style="margin-top:.5rem" onclick="location.href='/'">← Início</button>
</div>
<script>
function esc(s){var d=document.createElement('div');d.appendChild(document.createTextNode(s));return d.innerHTML}
fetch('/api/livros').then(r=>r.json()).then(function(d){
  var el=document.getElementById('lista');
  if(!d.livros||!d.livros.length){el.innerHTML='<p class="sem-livros">Nenhum livro disponível.</p>';return;}
  var h='';
  d.livros.forEach(function(l,i){
    h+='<a class="livro-card" href="/livro?id='+i+'" style="--cor-livro:'+esc(l.cor)+'">';
    h+='<span class="livro-ico">'+esc(l.emoji)+'</span>';
    h+='<div class="livro-info"><h3>'+esc(l.titulo)+'</h3><p>'+esc(l.subtitulo)+'</p></div>';
    h+='<span class="livro-pgs">'+l.nPaginas+' pág.</span>';
    h+='</a>';
  });
  el.innerHTML=h;
}).catch(function(){document.getElementById('lista').innerHTML='<p class="sem-livros">Erro ao carregar.</p>';});
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  PÁGINA: LIVRO DIGITAL
// ════════════════════════════════════════════════════════
const char PG_LIVRO[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=5,user-scalable=yes">
<title>Livro · Sala</title>
<style>
:root{--bg:#0b0c10;--s:#12131a;--b:#1e2030;--b2:#252840;--ac:#ff6b35;--ac2:#ffb347;--tx:#dde2f0;--mu:#5a6080;--blue:#4a9eff}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--tx);font-family:'IBM Plex Mono',monospace;min-height:100vh;display:flex;flex-direction:column;align-items:center;overscroll-behavior-y:none}
.school-bar{width:100%;max-width:540px;text-align:center;padding:.5rem 0 .2rem}
.school-name{font-family:'DM Serif Display',serif;font-size:.9rem;color:var(--blue)}
.school-dev{font-size:.42rem;color:#3a4060;letter-spacing:.18em;text-transform:uppercase;font-weight:600}
.topbar{width:100%;max-width:540px;display:flex;align-items:center;gap:.6rem;padding:.75rem 1rem .55rem;position:sticky;top:0;background:var(--bg);z-index:20;border-bottom:1px solid var(--b)}
.topbar h1{font-family:'DM Serif Display',serif;font-size:1rem;font-weight:400;font-style:italic;flex:1;color:var(--ac)}
.pg-info{font-size:.58rem;color:var(--mu);white-space:nowrap;letter-spacing:.08em}
.book{width:100%;max-width:540px;padding:.55rem .5rem 0;flex:1;display:flex;flex-direction:column}
.img-wrap{position:relative;width:100%;background:var(--s);border-radius:11px;border:1px solid var(--b);overflow:hidden}
#pgImg{width:100%;height:auto;display:block;transition:opacity .18s;min-height:180px;touch-action:pan-x pan-y pinch-zoom}
#pgImg.fading{opacity:0}
.spin-ov{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;background:var(--s);opacity:0;transition:opacity .18s;pointer-events:none}
.spin-ov.show{opacity:1}
.spinner{width:28px;height:28px;border:2px solid var(--b);border-top-color:var(--ac);border-radius:50%;animation:sp .6s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
.err-msg{color:#ff3e5e;font-size:.65rem;text-align:center;padding:.5rem;display:none;letter-spacing:.05em}
.dica-swipe{font-size:.5rem;color:var(--mu);text-align:center;padding:.3rem 0;letter-spacing:.07em}
.nav-row{display:flex;gap:.45rem;padding:.55rem .5rem 0;width:100%;max-width:540px}
.nbtn{flex:1;background:var(--s);border:1px solid var(--b);border-radius:9px;padding:.65rem;font-family:'IBM Plex Mono',monospace;font-size:.75rem;letter-spacing:.06em;color:var(--tx);cursor:pointer;text-align:center;transition:all .14s;text-transform:uppercase}
.nbtn:hover:not(:disabled){border-color:var(--b2);background:var(--b)}
.nbtn:active:not(:disabled){transform:scale(.96)}
.nbtn:disabled{opacity:.25;cursor:default}
.pgs-wrap{width:100%;max-width:540px;padding:.45rem .5rem 0;display:flex;gap:.22rem;flex-wrap:wrap;justify-content:center}
.pgt{width:28px;height:28px;border-radius:5px;background:var(--s);border:1px solid var(--b);font-size:.55rem;color:var(--mu);cursor:pointer;display:flex;align-items:center;justify-content:center;transition:all .13s;font-family:'IBM Plex Mono',monospace;font-weight:600}
.pgt:hover{border-color:var(--b2);color:var(--tx)}
.pgt.cur{background:var(--ac);border-color:transparent;color:#1a0a00}
.ir-row{display:flex;gap:.45rem;padding:.45rem .5rem 0;width:100%;max-width:540px;align-items:center}
.ir-row label{font-size:.56rem;color:var(--mu);letter-spacing:.06em;white-space:nowrap}
.ir-inp{flex:1;background:var(--s);border:1px solid var(--b);border-radius:7px;padding:.42rem .7rem;color:var(--tx);font-family:'IBM Plex Mono',monospace;font-size:.78rem;outline:none;text-align:center;max-width:65px;transition:border-color .2s}
.ir-inp:focus{border-color:var(--ac)}
.ir-btn{background:var(--ac);border:none;border-radius:7px;padding:.42rem .85rem;color:#1a0a00;font-family:'IBM Plex Mono',monospace;font-size:.72rem;font-weight:600;cursor:pointer}
.voltar-btn{display:block;margin:.75rem auto 0;background:transparent;border:1px solid var(--b);color:var(--mu);border-radius:8px;padding:.5rem 1.2rem;font-size:.6rem;cursor:pointer;letter-spacing:.07em;font-family:'IBM Plex Mono',monospace;text-transform:uppercase}
.voltar-btn:hover{border-color:var(--mu);color:var(--tx)}
</style></head><body>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="topbar">
  <h1 id="tituloLivro">📚 Livro</h1>
  <span class="pg-info" id="pgInfo">p.1</span>
</div>
<div class="book">
  <div class="img-wrap" id="imgWrap">
    <canvas id="pgImg" style="width:100%;height:auto;display:block;touch-action:pan-x pan-y pinch-zoom"></canvas>
    <div class="spin-ov show" id="spinOv"><div class="spinner"></div></div>
  </div>
  <p class="err-msg" id="errMsg">❌ Página não encontrada</p>
  <p class="dica-swipe">← Deslize · Pinça para zoom</p>
</div>
<div class="nav-row">
  <button class="nbtn" id="btnAnt" onclick="navegar(-1)" disabled>← Anterior</button>
  <button class="nbtn" id="btnProx" onclick="navegar(1)">Próxima →</button>
</div>
<div class="pgs-wrap" id="pgBtns"></div>
<div class="ir-row">
  <label>IR PARA</label>
  <input class="ir-inp" type="number" id="irInp" min="1" placeholder="1">
  <button class="ir-btn" onclick="irPara()">IR</button>
</div>
<button class="voltar-btn" onclick="location.href='/livros'">← Biblioteca</button>
<script>
var params=new URLSearchParams(location.search);
var livroId=parseInt(params.get('id')||'0');
var cur=1,total=20,prefixo='p',trocando=false;
fetch('/api/livros').then(r=>r.json()).then(function(d){
  if(d.livros&&d.livros[livroId]){
    var l=d.livros[livroId];
    total=l.nPaginas;prefixo=l.prefixo;
    document.getElementById('tituloLivro').textContent=l.emoji+' '+l.titulo;
    document.getElementById('irInp').max=total;
    document.title=l.titulo+' · Sala';
    construirBotoes();carregar(1);
  }
}).catch(function(){carregar(1);});
function construirBotoes(){
  var pb=document.getElementById('pgBtns');pb.innerHTML='';
  for(var i=1;i<=total;i++){
    var b=document.createElement('button');b.className='pgt'+(i===1?' cur':'');
    b.textContent=i;b.dataset.p=i;
    (function(p){b.onclick=function(){carregar(p)}})(i);
    pb.appendChild(b);
  }
}
function carregar(p){
  if(p<1||p>total||trocando)return;
  trocando=true;cur=p;
  var canvas=document.getElementById('pgImg');
  var sp=document.getElementById('spinOv'),er=document.getElementById('errMsg');
  sp.classList.add('show');er.style.display='none';

  // Aviso suave depois de 5s (não é erro, só informa que está demorando)
  var tidAviso=setTimeout(function(){
    er.style.display='block';
    er.style.color='var(--mu)';   // cinza, não vermelho
    er.textContent='⏳ Aguarde, carregando...';
  },5000);

  // Timeout real depois de 15s
  var tid=setTimeout(function(){
    clearTimeout(tidAviso);
    trocando=false;
    sp.classList.remove('show');
    er.style.display='block';
    er.style.color='#ff3e5e';     // aí sim vermelho
    er.textContent='❌ Página não carregou. Tente novamente.';
  },15000);

  fetch('/livro/img?id='+livroId+'&p='+p+'&t='+Date.now())
    .then(function(r){
      if(!r.ok)throw new Error('404');
      return r.blob();
    })
    .then(function(blob){
      var url=URL.createObjectURL(blob);
      var img=new Image();
      img.onload=function(){
        clearTimeout(tid);
        clearTimeout(tidAviso);    // ← limpar o aviso também
        canvas.width=img.naturalWidth;
        canvas.height=img.naturalHeight;
        var ctx=canvas.getContext('2d');
        ctx.drawImage(img,0,0);
        URL.revokeObjectURL(url);
        er.style.display='none';   // ← esconder qualquer aviso anterior
        sp.classList.remove('show');
        trocando=false;
      };
      img.onerror=function(){
        clearTimeout(tid);
        clearTimeout(tidAviso);
        URL.revokeObjectURL(url);
        sp.classList.remove('show');
        er.style.display='block';
        er.style.color='#ff3e5e';
        er.textContent='❌ Erro ao carregar imagem.';
        trocando=false;
      };
      img.src=url;
    })
    .catch(function(){
      clearTimeout(tid);
      clearTimeout(tidAviso);
      sp.classList.remove('show');
      er.style.display='block';
      er.style.color='#ff3e5e';
      er.textContent='❌ Página não encontrada.';
      trocando=false;
    });

  document.getElementById('pgInfo').textContent='p.'+p+' / '+total;
  document.getElementById('btnAnt').disabled=(p<=1);
  document.getElementById('btnProx').disabled=(p>=total);
  document.querySelectorAll('.pgt').forEach(function(b){
    b.classList.toggle('cur',parseInt(b.dataset.p)===p);
  });
  window.scrollTo({top:0,behavior:'smooth'});
 }
function navegar(d){carregar(cur+d)}
function irPara(){var v=parseInt(document.getElementById('irInp').value);if(v>=1&&v<=total)carregar(v);}
document.getElementById('irInp').addEventListener('keydown',function(e){if(e.key==='Enter')irPara()});
var tx=0,ty=0,ts=0,zoomed=false;
document.addEventListener('touchstart',function(e){
  if(e.touches.length>1){zoomed=true;return;}
  zoomed=false;
  tx=e.touches[0].screenX;ty=e.touches[0].screenY;ts=Date.now();
},{passive:true});
document.addEventListener('touchend',function(e){
  if(zoomed)return;
  if(e.changedTouches.length>1)return;
  var dx=e.changedTouches[0].screenX-tx,dy=e.changedTouches[0].screenY-ty,dt=Date.now()-ts;
  if(dt<400&&Math.abs(dx)>55&&Math.abs(dx)>Math.abs(dy)*1.4)navegar(dx<0?1:-1);
},{passive:true});
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  PÁGINA: ADMIN
// ════════════════════════════════════════════════════════
const char PG_ADMIN[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Admin · Sala</title><style>%CSS%
label{font-size:.6rem;color:var(--mu);letter-spacing:.08em;display:block;margin-bottom:.25rem;text-transform:uppercase}
.inp2{width:100%;background:var(--bg);border:1px solid var(--b2);border-radius:7px;padding:.56rem .76rem;color:var(--tx);font-family:'IBM Plex Mono',monospace;font-size:.72rem;outline:none;margin-bottom:.55rem;transition:border-color .2s}
.inp2:focus{border-color:var(--ac)}
textarea.inp2{min-height:68px;resize:vertical}
.fr{display:grid;grid-template-columns:1fr 1fr;gap:.45rem}
hr{border:none;border-top:1px solid var(--b);margin:.7rem 0}
select.inp2{cursor:pointer}
.tabs{display:flex;gap:0;background:var(--s);border:1px solid var(--b);border-radius:12px;padding:.3rem;margin-bottom:.85rem;overflow-x:auto}
.tab{flex:1;background:transparent;border:none;border-radius:8px;padding:.55rem .3rem;font-family:'IBM Plex Mono',monospace;font-size:.58rem;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--mu);cursor:pointer;transition:all .2s;white-space:nowrap;text-align:center}
.tab.ativo{background:var(--b2);color:var(--ac)}
.tab-pane{display:none}.tab-pane.ativo{display:block}
.tiles{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem;margin-bottom:.75rem}
.tile{background:var(--s);border:1px solid var(--b);border-radius:11px;padding:.75rem .9rem;position:relative;overflow:hidden}
.tile::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;border-radius:2px 2px 0 0}
.tile.t-on::before{background:var(--on)}.tile.t-off::before{background:var(--off)}.tile.t-ac::before{background:var(--ac)}.tile.t-blue::before{background:var(--blue)}.tile.t-yw::before{background:var(--yw)}
.tile .tl{font-size:.5rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.2rem}
.tile .tv{font-family:'DM Serif Display',serif;font-size:1.4rem;font-weight:400;line-height:1}
.tile .ts{font-size:.54rem;color:var(--mu);margin-top:.18rem}
.integ-table{width:100%;border-collapse:collapse;font-size:.58rem;margin-top:.45rem}
.integ-table th{color:var(--mu);letter-spacing:.09em;text-transform:uppercase;padding:.32rem .38rem;text-align:left;border-bottom:1px solid var(--b);font-size:.5rem}
.integ-table td{padding:.38rem .38rem;border-bottom:1px solid var(--b2);vertical-align:middle}
.integ-table tr:last-child td{border-bottom:none}
.badge{display:inline-block;font-size:.48rem;font-weight:600;padding:.16rem .42rem;border-radius:4px;letter-spacing:.06em;text-transform:uppercase}
.badge.verde{color:var(--on);background:#00ffa310;border:1px solid #00ffa330}
.badge.vermelho{color:var(--off);background:#ff3e5e10;border:1px solid #ff3e5e30}
.badge.amarelo{color:var(--yw);background:#ffd16610;border:1px solid #ffd16630}
.badge.cinza{color:var(--mu);background:var(--b2);border:1px solid var(--b)}
.badge.blue{color:var(--blue);background:#4a9eff10;border:1px solid #4a9eff30}
.btn-perfil{background:transparent;border:1px solid var(--b2);color:var(--blue);border-radius:5px;padding:.28rem .55rem;font-size:.56rem;cursor:pointer;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em}
.qdel{background:#1a0408;color:var(--off);border:1px solid #ff3e5e44;border-radius:5px;padding:.28rem .55rem;font-size:.56rem;cursor:pointer;font-family:'IBM Plex Mono',monospace;letter-spacing:.04em}
.qitem{background:var(--b);border-radius:8px;padding:.7rem;margin-bottom:.45rem;display:flex;gap:.45rem;align-items:flex-start;flex-wrap:wrap}
.qn{font-family:'DM Serif Display',serif;font-size:.9rem;color:var(--ac);min-width:1.3rem;font-style:italic}
.qt{font-size:.65rem;flex:1;line-height:1.45;color:var(--tx)}

.gab-block{display:flex;align-items:center;justify-content:space-between;background:var(--b);border-radius:8px;padding:.5rem .8rem;margin-bottom:.65rem}
.gab-lbl{font-size:.52rem;color:var(--mu);letter-spacing:.08em;text-transform:uppercase;margin-bottom:.12rem}
.gab-val{font-family:'DM Serif Display',serif;font-size:.9rem}
.act-row{display:flex;gap:.4rem;flex-wrap:wrap;margin-bottom:.4rem}
.act-row .btn{flex:1;min-width:0}
.nenhum{font-size:.6rem;color:var(--mu);text-align:center;padding:.75rem;letter-spacing:.06em}
.chips{display:flex;flex-wrap:wrap;gap:.28rem;margin:.45rem 0}
.pchip{display:inline-flex;align-items:center;gap:.22rem;background:#00ffa310;border:1px solid #00ffa330;border-radius:4px;padding:.16rem .42rem;font-size:.5rem;color:var(--on);letter-spacing:.04em}
</style></head><body>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="wrap">
  <div class="hd"><h1><em>Admin</em></h1><p>Controle da sala</p></div>
  <div class="tiles">
    <div class="tile t-blue"><div class="tl">Alunos</div><div class="tv" id="tAlunos">0</div><div class="ts">na sala</div></div>
    <div class="tile t-ac"><div class="tl">Prova</div><div class="tv" id="tProva" style="font-size:1rem;padding-top:.2rem">—</div><div class="ts" id="tProvaSub">—</div></div>
    <div class="tile t-yw"><div class="tl">Questões</div><div class="tv" id="tQ">0</div><div class="ts" id="tQSub">gabarito oculto</div></div>
    <div class="tile t-on"><div class="tl">Responderam</div><div class="tv" id="tResp">0</div><div class="ts">alunos</div></div>
  </div>
  <div class="tabs">
    <button class="tab ativo" onclick="aba('geral')">Geral</button>
    <button class="tab" onclick="aba('quiz')">Quiz</button>
    <button class="tab" onclick="aba('alunos')">Alunos</button>
    <button class="tab" onclick="aba('historico')">Histórico</button>
    <button class="tab" onclick="aba('materiais')">Materiais</button>
  </div>

  <!-- ABA: GERAL -->
  <div class="tab-pane ativo" id="pane-geral">
    <div class="card">
      <div class="sect">📋 Prova</div>
      <div id="listaPresenca" style="margin-bottom:.65rem"></div>
      <div class="act-row">
        <button class="btn btn-verde btn-sm" onclick="cmd('abrirProva')">▶ Abrir</button>
        <button class="btn btn-danger btn-sm" onclick="cmd('fecharProva')">🔒 Fechar</button>
      </div>
      <button class="btn btn-warn btn-sm" style="width:100%;margin-top:.4rem" onclick="cmd('resetQuiz')">↺ Resetar tudo</button>
      <a href="/dashboard" style="display:block;text-decoration:none;margin-top:.4rem"><button class="btn btn-blue btn-sm" style="width:100%">📺 Painel ao vivo →</button></a>
    </div>
  </div>

  <!-- ABA: QUIZ -->
  <div class="tab-pane" id="pane-quiz">
    <div class="card">
      <div class="sect">🧠 Gabarito</div>
      <div class="gab-block">
        <div><div class="gab-lbl">Estado</div><div class="gab-val" id="gabStatus">—</div></div>
      </div>
      <div class="g2">
        <button class="btn btn-verde btn-sm" onclick="cmd('liberarGabarito')">👁 Revelar</button>
        <button class="btn btn-danger btn-sm" onclick="cmd('ocultarGabarito')">🙈 Ocultar</button>
      </div>
    </div>
    <div class="card">
      <div class="sect">📝 Questões</div>
      <div id="qList"><p class="nenhum">Sem questões.</p></div>
      <hr>
      <label>Enunciado</label>
      <textarea class="inp2" id="qE" placeholder="Digite a pergunta..." maxlength="119" rows="2"></textarea>
      <div class="fr">
        <div><label>A</label><input class="inp2" type="text" id="qA" maxlength="47"></div>
        <div><label>B</label><input class="inp2" type="text" id="qB" maxlength="47"></div>
        <div><label>C (opcional)</label><input class="inp2" type="text" id="qC" maxlength="47"></div>
        <div><label>D (opcional)</label><input class="inp2" type="text" id="qD" maxlength="47"></div>
      </div>
      <label>Gabarito</label>
      <select class="inp2" id="qG">
        <option value="">Sem gabarito</option>
        <option>A</option><option>B</option><option>C</option><option>D</option>
      </select>
      <label>Observação explicativa</label>
      <input class="inp2" type="text" id="qObs" maxlength="159" placeholder="Ex: Isolando x: 2x=8, x=4.">
      <button class="btn btn-ac btn-sm" style="width:100%" onclick="addQ()">+ Adicionar questão</button>
      <div class="g2" style="margin-top:.45rem">
        <button class="btn btn-neu btn-sm" onclick="cmd('resetQuizResp')">↺ Limpar respostas</button>
        <button class="btn btn-danger btn-sm" onclick="cmd('limparQuiz')">🗑 Limpar tudo</button>
      </div>
    </div>
  </div>

  <!-- ABA: ALUNOS -->
  <div class="tab-pane" id="pane-alunos">
    <div class="card">
      <div class="sect">👥 Lista de Alunos</div>
      <div id="alunosTabela"><p class="nenhum">Nenhum aluno ainda.</p></div>
    </div>
  </div>

  <!-- ABA: HISTORICO -->
  <div class="tab-pane" id="pane-historico">
    <div class="card">
      <div class="sect">🗂 Atividades Arquivadas</div>
      <div id="histLista"><p class="nenhum">Carregando...</p></div>
    </div>
    <div class="card" id="histDetalhe" style="display:none">
      <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:.65rem">
        <div class="sect" style="margin:0" id="histDetTitulo">Detalhe</div>
        <button class="btn-perfil" onclick="fecharDetalhe()">X Fechar</button>
      </div>
      <div id="histDetConteudo"></div>
    </div>
  </div>

  <!-- ABA: MATERIAIS -->
  <div class="tab-pane" id="pane-materiais">
    <div class="card">
      <div class="sect">📁 Materiais de Referência</div>
      <p style="font-size:.6rem;color:var(--mu);line-height:1.6;margin-bottom:.85rem">Imagens do quadro, PDFs e documentos para usar como referência durante a aula.</p>
      <button class="btn btn-ac btn-sm" style="width:100%" onclick="abrirModalMaterial()">+ Adicionar material</button>
    </div>
    <div class="card">
      <div class="sect">📋 Lista de materiais</div>
      <div id="listaMateriais">
        <div style="text-align:center;padding:1.5rem 0">
          <div style="font-size:2rem;margin-bottom:.5rem;opacity:.3">📂</div>
          <p style="font-size:.6rem;color:var(--mu);letter-spacing:.06em">Nenhum material adicionado ainda.</p>
          <p style="font-size:.54rem;color:var(--mu2);margin-top:.3rem">Em breve: armazenamento no ESP.</p>
        </div>
      </div>
    </div>
  </div>
</div>

<!-- inputs de arquivo — um por tipo, label associado troca o for conforme seleção -->
<input type="file" id="inputImg" style="position:absolute;opacity:0;width:0;height:0;pointer-events:none" accept="image/*" onchange="onArquivoSelecionado()">
<input type="file" id="inputPdf" style="position:absolute;opacity:0;width:0;height:0;pointer-events:none" accept=".pdf,application/pdf" onchange="onArquivoSelecionado()">
<input type="file" id="inputDoc" style="position:absolute;opacity:0;width:0;height:0;pointer-events:none" accept=".doc,.docx,.odt,.txt,application/msword,application/vnd.openxmlformats-officedocument.wordprocessingml.document" onchange="onArquivoSelecionado()">

<!-- MODAL: ADICIONAR MATERIAL -->
<div class="ov" id="ovMaterial">
  <div class="modal" style="max-width:340px">
    <h2>Novo material</h2>
    <p style="font-size:.6rem;color:var(--mu);margin-bottom:.9rem;line-height:1.5">Escolha o tipo e toque em Selecionar arquivo.</p>

    <label style="font-size:.55rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;display:block;margin-bottom:.45rem">Tipo</label>
    <div style="display:flex;gap:.4rem;margin-bottom:.85rem">
      <button id="tipo-img" onclick="selecionarTipo('img')" style="flex:1;border-radius:8px;border:1px solid var(--b);font-family:'IBM Plex Mono',monospace;font-size:.62rem;padding:.55rem .2rem;cursor:pointer;transition:all .2s;background:var(--ac);color:#1a0a00;border-color:transparent">📷 Imagem</button>
      <button id="tipo-pdf" onclick="selecionarTipo('pdf')" style="flex:1;border-radius:8px;border:1px solid var(--b);font-family:'IBM Plex Mono',monospace;font-size:.62rem;padding:.55rem .2rem;cursor:pointer;transition:all .2s;background:var(--b2);color:var(--mu)">📄 PDF</button>
      <button id="tipo-doc" onclick="selecionarTipo('doc')" style="flex:1;border-radius:8px;border:1px solid var(--b);font-family:'IBM Plex Mono',monospace;font-size:.62rem;padding:.55rem .2rem;cursor:pointer;transition:all .2s;background:var(--b2);color:var(--mu)">📝 Doc</button>
    </div>

    <!-- área de arquivo selecionado -->
    <div id="arquivoInfo" style="background:var(--b);border-radius:8px;padding:.6rem .75rem;margin-bottom:.6rem;font-size:.6rem;color:var(--mu);display:flex;align-items:center;justify-content:space-between;gap:.5rem">
      <span id="arquivoNomeExib" style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">Nenhum arquivo</span>
      <label id="btnSelecionar" for="inputImg" style="background:var(--ac);color:#1a0a00;border:none;border-radius:6px;padding:.32rem .65rem;font-family:'IBM Plex Mono',monospace;font-size:.58rem;font-weight:600;cursor:pointer;white-space:nowrap;display:inline-block;line-height:1.8">Selecionar</label>
    </div>

    <label style="font-size:.55rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;display:block;margin-bottom:.35rem">Descrição (opcional)</label>
    <input type="text" id="nomeMaterial" class="inp" placeholder="Ex: Foto do quadro — 3ª aula" maxlength="60" style="margin-bottom:.7rem">

    <p class="merr" id="erroMaterial"></p>
    <div class="mbtns">
      <button class="mok" onclick="confirmarMaterial()">ADICIONAR</button>
      <button class="mno" onclick="fecharModalMaterial()">✕</button>
    </div>
  </div>
</div>

<div id="toast"></div>
<script>
var ultimoD={};
function esc(s){if(!s)return'';var d=document.createElement('div');d.appendChild(document.createTextNode(s));return d.innerHTML}
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.classList.add('show');setTimeout(()=>e.classList.remove('show'),2200)}
function aba(id){
  document.querySelectorAll('.tab').forEach(function(t,i){t.classList.toggle('ativo',['geral','quiz','alunos','historico','materiais'][i]===id)});
  document.querySelectorAll('.tab-pane').forEach(function(p){p.classList.remove('ativo')});
  document.getElementById('pane-'+id).classList.add('ativo');
  if(id==='historico') carregarHistorico();
}

// ── Modal Materiais ──
var tipoAtual='img';
function _inputId(t){return 'input'+(t==='img'?'Img':t==='pdf'?'Pdf':'Doc');}
function abrirModalMaterial(){
  tipoAtual='img';
  _atualizarTipos();
  ['Img','Pdf','Doc'].forEach(function(s){document.getElementById('input'+s).value='';});
  document.getElementById('arquivoNomeExib').textContent='Nenhum arquivo';
  document.getElementById('nomeMaterial').value='';
  document.getElementById('erroMaterial').textContent='';
  document.getElementById('ovMaterial').classList.add('open');
}
function fecharModalMaterial(){
  document.getElementById('ovMaterial').classList.remove('open');
}
function selecionarTipo(t){
  tipoAtual=t;
  _atualizarTipos();
  document.getElementById('btnSelecionar').setAttribute('for',_inputId(t));
  document.getElementById(_inputId(t)).value='';
  document.getElementById('arquivoNomeExib').textContent='Nenhum arquivo';
}
function _atualizarTipos(){
  ['img','pdf','doc'].forEach(function(t){
    var b=document.getElementById('tipo-'+t);
    var on=t===tipoAtual;
    b.style.background=on?'var(--ac)':'var(--b2)';
    b.style.color=on?'#1a0a00':'var(--mu)';
    b.style.borderColor=on?'transparent':'var(--b)';
  });
  document.getElementById('btnSelecionar').setAttribute('for',_inputId(tipoAtual));
}
function onArquivoSelecionado(){
  var f=document.getElementById(_inputId(tipoAtual)).files[0];
  document.getElementById('arquivoNomeExib').textContent=f?f.name:'Nenhum arquivo';
  document.getElementById('erroMaterial').textContent='';
}
function confirmarMaterial(){
  var f=document.getElementById(_inputId(tipoAtual)).files[0];
  var err=document.getElementById('erroMaterial');
  if(!f){err.textContent='Selecione um arquivo primeiro.';return;}
  err.textContent='';
  var nome=document.getElementById('nomeMaterial').value.trim()||f.name;
  toast('Material "'+nome+'" recebido (armazenamento em breve) ✓');
  fecharModalMaterial();
}
function fmtTempo(ms){if(!ms||ms<=0)return'—';if(ms<1000)return ms+'ms';return Math.round(ms/1000)+'s';}

function renderAlunosTabela(d){
  var el=document.getElementById('alunosTabela');
  if(!d.voters||!d.voters.length){el.innerHTML='<p class="nenhum">Nenhum aluno.</p>';return;}
  var h='<table class="integ-table">';
  h+='<tr><th>Aluno</th>';
  for(var q=0;q<(d.nQ||0);q++) h+='<th style="text-align:center">Q'+(q+1)+'</th>';
  h+='<th></th></tr>';
  d.voters.forEach(function(v,vi){
    var nm=esc(v.nome&&v.nome!==v.ip?v.nome:('Aluno '+(vi+1)));
    var resp=0;for(var k=0;k<(d.nQ||0);k++){if(v.resp&&v.resp[k]&&v.resp[k]!=='0')resp++;}
    var bc=v.quizIniciado?'verde':'cinza',bt=v.quizIniciado?'ATIVO':'PRESENTE';
    h+='<tr><td><b style="font-size:.6rem">'+nm+'</b><br><span class="badge '+bc+'">'+bt+'</span></td>';
    for(var k=0;k<(d.nQ||0);k++){
      var r=v.resp&&v.resp[k]&&v.resp[k]!=='0'?v.resp[k]:'—';
      var gab=d.gabs&&d.gabs[k]&&d.gabs[k]!=='0'?d.gabs[k]:null;
      var cor='color:var(--mu)';
      if(r!=='—'&&gab){cor=r===gab?'color:var(--on)':'color:var(--off)';}
      h+='<td style="text-align:center;font-size:.58rem;'+cor+'">'+r+'</td>';
    }
    h += '<td style="display:flex;gap:.3rem;flex-wrap:wrap">';
    h += '<button class="btn-perfil" onclick="verPerfil('+vi+')">⏱</button>';
    h += '<button class="btn-perfil" style="color:var(--yw)" onclick="delAluno(\''+esc(v.ip)+'\')">✕</button>';
    h += '<button class="btn-perfil" style="color:var(--ac)" onclick="tornarElegivel(\''+esc(v.ip)+'\')">'+(v.elegivel?'🔓':'🔒')+'</button>';
    h += '</td>';
  });
  h+='</table>';
  el.innerHTML=h;
}

function atualizar(d){
  ultimoD=d;
  document.getElementById('tAlunos').textContent=d.cl||0;
  var gl=d.gabLiberado||false;
  var gsEl=document.getElementById('gabStatus');
  gsEl.textContent=gl?'VISÍVEL':'OCULTO';gsEl.style.color=gl?'var(--on)':'var(--off)';
  document.getElementById('tQ').textContent=d.nQ||0;
  document.getElementById('tQSub').textContent=gl?'gabarito visível':'gabarito oculto';
  var resp=0;
  (d.voters||[]).forEach(function(v){if((v.resp||[]).some(function(r){return r&&r!=='0';}))resp++;});
  document.getElementById('tResp').textContent=resp;
  var pStatus=d.provaAberta?(d.provaFechada?'FECHADA':'ABERTA'):'AGUARDANDO';
  var pCor=d.provaAberta?(d.provaFechada?'var(--off)':'var(--on)'):'var(--yw)';
  document.getElementById('tProva').textContent=pStatus;
  document.getElementById('tProva').style.color=pCor;
  document.getElementById('tProvaSub').textContent=d.provaAberta?(d.provaFechada?'inscrições encerradas':'recebendo alunos'):'não iniciada';
  // Presença
  var pEl=document.getElementById('listaPresenca');
  var presentes=(d.voters||[]).filter(function(v){return v.quizIniciado;});
  if(presentes.length){
    var h='<div class="chips">';
    presentes.forEach(function(v,i){
      h+='<span class="pchip">✓ '+esc(v.nome&&v.nome!==v.ip?v.nome:('Aluno '+(i+1)))+'</span>';
    });
    h+='</div>';
    pEl.innerHTML=h;
  } else {
    pEl.innerHTML='<p style="font-size:.6rem;color:var(--mu);margin-bottom:.5rem">Nenhum aluno confirmou presença.</p>';
  }
  // Quiz list
  var ql=document.getElementById('qList');var h='';
  if(d.qs&&d.qs.length){
    d.qs.forEach(function(q,i){
      h+='<div class="qitem"><span class="qn">'+(i+1)+'</span>';
      h+='<div style="flex:1;min-width:0"><div class="qt">'+esc(q.t)+(q.g&&q.g!='0'?'<b style="color:var(--on)"> ['+q.g+']</b>':'')+'</div>';
      if(q.obs)h+='<div style="font-size:.56rem;color:var(--ac2);margin-top:.2rem;font-style:italic">↳ '+esc(q.obs)+'</div>';
      h+='<div style="display:flex;gap:.35rem;margin-top:.45rem">';
      h+='<button class="btn-perfil" onclick="editarQ('+i+')">✎ Editar</button>';
      h+='<button class="qdel" onclick="delQ('+i+')">✕ Apagar</button></div></div>';
    });
  } else h='<p class="nenhum">Sem questões.</p>';
  ql.innerHTML=h;
  renderAlunosTabela(d);
}
var _editIdx=-1;
function editarQ(i){
  var q=ultimoD.qs[i];if(!q)return;
  _editIdx=i;
  document.getElementById('qE').value=q.t||'';
  document.getElementById('qA').value=q.a||'';
  document.getElementById('qB').value=q.b||'';
  document.getElementById('qC').value=q.c||'';
  document.getElementById('qD').value=q.d||'';
  document.getElementById('qG').value=q.g||'';
  document.getElementById('qObs').value=q.obs||'';
  document.getElementById('editIdx').textContent=(i+1);
  document.getElementById('editBanner').style.display='block';
  document.getElementById('btnAddEdit').textContent='✎ Salvar edição';
}
function cancelarEdicao(){
  _editIdx=-1;
  document.getElementById('editBanner').style.display='none';
  document.getElementById('btnAddEdit').textContent='+ Adicionar questão';
  ['qE','qA','qB','qC','qD','qObs'].forEach(id=>document.getElementById(id).value='');
  document.getElementById('qG').value='';
}

var msgs={resetQuiz:'↺ Prova resetada',resetQuizResp:'↺ Respostas limpas',limparQuiz:'🗑 Quiz limpo',liberarGabarito:'👁 Gabarito visível',ocultarGabarito:'🙈 Gabarito oculto',abrirProva:'▶ Prova aberta',fecharProva:'🔒 Encerrada'};
function cmd(a){fetch('/admin/cmd?a='+a).then(r=>r.json()).then(d=>{toast(msgs[a]||'OK');atualizar(d)}).catch(()=>toast('Erro.'))}
function addOuEditQ(){
  var e=document.getElementById('qE').value.trim(),a=document.getElementById('qA').value.trim(),b=document.getElementById('qB').value.trim();
  if(!e||!a||!b){toast('Enunciado + A e B obrigatórios.');return}
  var u='/admin/addQ?e='+encodeURIComponent(e)+'&a='+encodeURIComponent(a)+'&b='+encodeURIComponent(b);
  u+='&c='+encodeURIComponent(document.getElementById('qC').value.trim());
  u+='&d='+encodeURIComponent(document.getElementById('qD').value.trim());
  u+='&g='+encodeURIComponent(document.getElementById('qG').value);
  u+='&obs='+encodeURIComponent(document.getElementById('qObs').value.trim());
  fetch(u).then(r=>r.json()).then(d=>{
    if(d.erro){toast(d.erro);return}
    toast('Questão adicionada ✓');
    ['qE','qA','qB','qC','qD','qObs'].forEach(id=>document.getElementById(id).value='');
    document.getElementById('qG').value='';
    atualizar(d);
  }).catch(()=>toast('Erro.'))
}
function delQ(i){fetch('/admin/delQ?i='+i).then(r=>r.json()).then(d=>{toast('Questão removida.');atualizar(d)}).catch(()=>toast('Erro.'))}
function poll(){fetch('/admin/data').then(r=>r.json()).then(atualizar).catch(()=>{})}
poll();setInterval(poll,2500);

function delAluno(ip){
  if(!confirm('Remover aluno?')) return;
  fetch('/admin/delAluno?ip='+encodeURIComponent(ip))
    .then(r=>r.json()).then(d=>{toast('Removido.');atualizar(d);})
    .catch(()=>toast('Erro.'));
}
function tornarElegivel(ip){
  fetch('/admin/elegivel?ip='+encodeURIComponent(ip))
    .then(r=>r.json()).then(d=>{toast('Conta liberada para novo dispositivo.');atualizar(d);})
    .catch(()=>toast('Erro.'));
}

function carregarHistorico(){
  var el=document.getElementById('histLista');
  el.innerHTML='<p class="nenhum">Carregando...</p>';
  fetch('/admin/historico').then(r=>r.json()).then(function(d){
    var ats=d.atividades||[];
    if(!ats.length){el.innerHTML='<p class="nenhum">Nenhuma atividade arquivada.</p>';return;}
    var h='';
    ats.forEach(function(a){
      h+='<div style="display:flex;align-items:center;justify-content:space-between;padding:.5rem 0;border-bottom:1px solid var(--b)">';
      h+='<div>';
      h+='<div style="font-size:.65rem;color:var(--tx);font-weight:600">'+esc(a.nome||'Atividade')+'</div>';
      h+='<div style="font-size:.52rem;color:var(--mu);margin-top:.15rem">'+( a.nQ||0)+' questões · '+(a.nAlunos||0)+' alunos</div>';
      h+='</div>';
      h+='<div style="display:flex;gap:.35rem">';
      h+='<button class="btn-perfil" onclick="verHistorico('+a.id+',\''+esc(a.nome||'Atividade')+'\')">Ver</button>';
      h+='<button class="qdel" onclick="delHistorico('+a.id+')">✕</button>';
      h+='</div>';
      h+='</div>';
    });
    el.innerHTML=h;
  }).catch(function(){el.innerHTML='<p class="nenhum" style="color:var(--off)">Erro ao carregar.</p>';});
}

function verHistorico(id, nome){
  fetch('/admin/histData?id='+id).then(r=>r.json()).then(function(d){
    document.getElementById('histDetTitulo').textContent=nome;
    var nQ=d.nQ||0;
    var alunos=d.alunos||[];
    var h='<table class="integ-table">';
    h+='<tr><th>Aluno</th>';
    for(var q=0;q<nQ;q++) h+='<th style="text-align:center">Q'+(q+1)+'</th>';
    h+='<th style="text-align:center">Acertos</th></tr>';
    alunos.forEach(function(a){
      var rs=a.resp||[];
      var qs=d.qs||[];
      var acertos=0;
      h+='<tr><td style="font-size:.6rem">'+esc(a.nome)+'</td>';
      for(var k=0;k<nQ;k++){
        var r=rs[k]||'—';
        var gab=qs[k]?qs[k].gab:'';
        var cor='color:var(--mu)';
        if(r!=='—'&&gab){
          if(r===gab){cor='color:var(--on)';acertos++;}
          else cor='color:var(--off)';
        }
        h+='<td style="text-align:center;font-size:.58rem;'+cor+'">'+esc(r)+'</td>';
      }
      var pct=nQ>0?Math.round(acertos/nQ*100):0;
      h+='<td style="text-align:center;font-size:.62rem;color:var(--blue)">'+acertos+'/'+nQ+' ('+pct+'%)</td>';
      h+='</tr>';
    });
    h+='</table>';
    document.getElementById('histDetConteudo').innerHTML=h;
    document.getElementById('histDetalhe').style.display='block';
  }).catch(function(){toast('Erro ao carregar detalhe.');});
}

function fecharDetalhe(){
  document.getElementById('histDetalhe').style.display='none';
  document.getElementById('histDetConteudo').innerHTML='';
}

function delHistorico(id){
  if(!confirm('Apagar esta atividade do histórico?')) return;
  fetch('/admin/histDel?id='+id).then(r=>r.json()).then(function(){
    toast('Atividade removida.');
    carregarHistorico();
    fecharDetalhe();
  }).catch(function(){toast('Erro.');});
}

function verPerfil(vi){
  var d=ultimoD;if(!d||!d.voters||!d.voters[vi])return;
  var v=d.voters[vi];
  var nm=v.nome&&v.nome!==v.ip?v.nome:('Aluno '+(vi+1));
  var h='<div style="position:fixed;inset:0;background:#0009;z-index:999;display:flex;align-items:center;justify-content:center" onclick="this.remove()">';
  h+='<div style="background:var(--s);border:1px solid var(--b);border-radius:14px;padding:1.2rem;width:92%;max-width:360px;max-height:85vh;overflow-y:auto" onclick="event.stopPropagation()">';
  h+='<div style="font-family:\'DM Serif Display\',serif;font-size:1.1rem;margin-bottom:.4rem;color:var(--ac)">'+esc(nm)+'</div>';
  h+='<div style="font-size:.52rem;color:var(--mu);margin-bottom:.85rem;letter-spacing:.06em">Perfil de respostas — tempo por questão</div>';
  h+='<table style="width:100%;border-collapse:collapse;font-size:.62rem">';
  h+='<tr>';
  h+='<th style="text-align:left;color:var(--mu);padding:.28rem .35rem;border-bottom:1px solid var(--b);font-size:.5rem;letter-spacing:.08em;text-transform:uppercase">Q</th>';
  h+='<th style="text-align:center;color:var(--mu);padding:.28rem .35rem;border-bottom:1px solid var(--b);font-size:.5rem;letter-spacing:.08em;text-transform:uppercase">Resp.</th>';
  h+='<th style="text-align:center;color:var(--mu);padding:.28rem .35rem;border-bottom:1px solid var(--b);font-size:.5rem;letter-spacing:.08em;text-transform:uppercase">Tempo</th>';
  h+='<th style="text-align:center;color:var(--mu);padding:.28rem .35rem;border-bottom:1px solid var(--b);font-size:.5rem;letter-spacing:.08em;text-transform:uppercase">Gab</th>';
  h+='</tr>';
  var nQ=d.nQ||0;
  for(var i=0;i<nQ;i++){
    var r=v.resp&&v.resp[i]&&v.resp[i]!=='0'?v.resp[i]:'—';
    var t=v.tempos&&v.tempos[i]>0&&v.tempos[i]<300000?fmtTempo(v.tempos[i]):'—';
    var gab=d.gabs&&d.gabs[i]&&d.gabs[i]!=='0'?d.gabs[i]:'';
    var ok=gab&&r!=='—'?(r===gab):null;
    var rCor=ok===null?'var(--tx)':(ok?'var(--on)':'var(--off)');
    var tCor=ok===null?'var(--ac2)':(ok?'var(--on)':'var(--off)');
    h+='<tr>';
    h+='<td style="padding:.32rem .35rem;border-bottom:1px solid var(--b2);color:var(--mu);font-weight:600">Q'+(i+1)+'</td>';
    h+='<td style="text-align:center;padding:.32rem .35rem;border-bottom:1px solid var(--b2);color:'+rCor+';font-weight:600">'+r+'</td>';
    h+='<td style="text-align:center;padding:.32rem .35rem;border-bottom:1px solid var(--b2);color:'+tCor+'">'+t+'</td>';
    h+='<td style="text-align:center;padding:.32rem .35rem;border-bottom:1px solid var(--b2);color:var(--on)">'+(gab||'—')+'</td>';
    h+='</tr>';
  }
  h+='</table>';
  // Resumo
  var acertos=0,respondidas=0,totalTempo=0,contT=0;
  for(var i=0;i<nQ;i++){
    var r=v.resp&&v.resp[i]&&v.resp[i]!=='0'?v.resp[i]:null;
    if(r)respondidas++;
    var gab=d.gabs&&d.gabs[i]&&d.gabs[i]!=='0'?d.gabs[i]:null;
    if(r&&gab&&r===gab)acertos++;
    if(v.tempos&&v.tempos[i]>0&&v.tempos[i]<300000){totalTempo+=v.tempos[i];contT++;}
  }
  var tmMedio=contT>0?fmtTempo(Math.round(totalTempo/contT)):'—';
  h+='<div style="display:flex;gap:.4rem;margin-top:.8rem;flex-wrap:wrap">';
  h+='<div style="flex:1;background:var(--b);border-radius:7px;padding:.5rem .7rem;text-align:center"><div style="font-size:.48rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.2rem">Respondidas</div><div style="font-family:\'DM Serif Display\',serif;font-size:1.2rem;color:var(--blue)">'+respondidas+'/'+nQ+'</div></div>';
  if(d.gabLiberado||acertos>0){
    h+='<div style="flex:1;background:var(--b);border-radius:7px;padding:.5rem .7rem;text-align:center"><div style="font-size:.48rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.2rem">Acertos</div><div style="font-family:\'DM Serif Display\',serif;font-size:1.2rem;color:var(--on)">'+acertos+'/'+nQ+'</div></div>';
  }
  h+='<div style="flex:1;background:var(--b);border-radius:7px;padding:.5rem .7rem;text-align:center"><div style="font-size:.48rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.2rem">Tempo médio</div><div style="font-family:\'DM Serif Display\',serif;font-size:1.2rem;color:var(--yw)">'+tmMedio+'</div></div>';
  h+='</div>';
  h+='<p style="font-size:.52rem;color:var(--mu);margin-top:.75rem;text-align:center;letter-spacing:.05em">Toque fora para fechar</p>';
  h+='</div></div>';
  document.body.insertAdjacentHTML('beforeend',h);
}
function fmtTempo(ms){if(!ms||ms<=0)return'—';if(ms<1000)return ms+'ms';var s=Math.round(ms/1000);if(s<60)return s+'s';return Math.floor(s/60)+'m '+( s%60)+'s';}
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  PÁGINA: DASHBOARD AO VIVO
// ════════════════════════════════════════════════════════
const char PG_DASH[] PROGMEM = R"X(<!DOCTYPE html>
<html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Painel · Sala</title>
<style>
:root{--bg:#0b0c10;--s:#12131a;--b:#1e2030;--b2:#252840;--on:#00ffa3;--off:#ff3e5e;--ac:#ff6b35;--ac2:#ffb347;--blue:#4a9eff;--tx:#dde2f0;--mu:#5a6080;--yw:#ffd166}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'IBM Plex Mono',monospace;background:var(--bg);color:var(--tx);min-height:100vh;padding:.65rem .65rem 3rem}
body::before{content:'';position:fixed;inset:0;background:radial-gradient(ellipse 60% 35% at 80% 5%,#ff6b3506,transparent);pointer-events:none;z-index:0}
.school-bar{text-align:center;padding:.4rem 0 .2rem;position:relative;z-index:1}
.school-name{font-family:'DM Serif Display',serif;font-size:1rem;color:var(--blue)}
.school-dev{font-size:.44rem;color:#3a4060;letter-spacing:.18em;text-transform:uppercase;font-weight:600;margin-top:.1rem}
.topbar{position:relative;z-index:1;display:flex;align-items:center;justify-content:space-between;padding:.35rem 0 .65rem;flex-wrap:wrap;gap:.35rem}
.topbar h1{font-family:'DM Serif Display',serif;font-size:1.05rem;font-weight:400;font-style:italic;color:var(--ac)}
.live{display:flex;align-items:center;gap:.32rem;font-size:.55rem;color:var(--mu);letter-spacing:.1em}
.ldot{width:6px;height:6px;border-radius:50%;background:var(--on);animation:pulse 1.6s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.2;transform:scale(.5)}}
.metrics{position:relative;z-index:1;display:grid;grid-template-columns:repeat(4,1fr);gap:.45rem;margin-bottom:.65rem}
@media(max-width:480px){.metrics{grid-template-columns:repeat(2,1fr)}}
.met{background:var(--s);border:1px solid var(--b);border-radius:11px;padding:.7rem .85rem;position:relative;overflow:hidden}
.met::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;border-radius:2px 2px 0 0}
.met.c-on::before{background:var(--on)}.met.c-ac::before{background:var(--ac)}.met.c-yw::before{background:var(--yw)}.met.c-off::before{background:var(--off)}.met.c-blue::before{background:var(--blue)}
.met .ml{font-size:.48rem;color:var(--mu);letter-spacing:.1em;text-transform:uppercase;margin-bottom:.2rem}
.met .mv{font-family:'DM Serif Display',serif;font-size:1.6rem;font-weight:400}
.met .ms{font-size:.48rem;color:var(--mu);margin-top:.15rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.main{position:relative;z-index:1;display:grid;grid-template-columns:1fr 1fr;gap:.55rem}
@media(max-width:560px){.main{grid-template-columns:1fr}}
.panel{background:var(--s);border:1px solid var(--b);border-radius:13px;padding:.85rem .95rem;margin-bottom:.55rem}
.ptitle{font-size:.56rem;letter-spacing:.14em;text-transform:uppercase;color:var(--ac);margin-bottom:.65rem;padding-bottom:.38rem;border-bottom:1px solid var(--b);display:flex;align-items:center;justify-content:space-between}
.pcount{color:var(--mu);font-size:.52rem;letter-spacing:.04em}
.vrow{display:flex;align-items:center;gap:.45rem;margin-bottom:.45rem}
.vname{font-size:.56rem;color:var(--mu);width:76px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex-shrink:0}
.vtrack{flex:1;height:7px;background:var(--b);border-radius:4px;overflow:hidden}
.vfill{height:100%;border-radius:4px;transition:width .8s cubic-bezier(.4,0,.2,1)}
.vpct{font-size:.55rem;color:var(--tx);width:26px;text-align:right;flex-shrink:0}
.qblock{margin-bottom:.85rem;padding-bottom:.85rem;border-bottom:1px solid var(--b)}
.qblock:last-child{margin-bottom:0;padding-bottom:0;border-bottom:none}
.qenun{font-size:.62rem;line-height:1.45;color:var(--tx);margin-bottom:.55rem;flex:1}
.qtopo{display:flex;align-items:flex-start;justify-content:space-between;gap:.45rem;margin-bottom:.55rem}
.qbadge{flex-shrink:0;font-size:.5rem;font-weight:600;padding:.16rem .42rem;border-radius:4px;border:1px solid;white-space:nowrap;letter-spacing:.05em}
.qbadge.verde{color:var(--on);border-color:#00ffa330;background:#00ffa308}
.qbadge.amarelo{color:var(--yw);border-color:#ffd16630;background:#ffd16608}
.qbadge.vermelho{color:var(--off);border-color:#ff3e5e30;background:#ff3e5e08}
.qbadge.cinza{color:var(--mu);border-color:var(--b);background:transparent}
.alt-grid{display:flex;flex-direction:column;gap:.28rem;margin-bottom:.45rem}
.alt-row{display:flex;align-items:center;gap:.38rem}
.alt-ltr{font-family:'DM Serif Display',serif;font-size:.75rem;font-style:italic;width:13px;flex-shrink:0;text-align:center}
.alt-bar-wrap{flex:1;height:13px;background:var(--b);border-radius:3px;overflow:hidden;position:relative}
.alt-bar{height:100%;border-radius:3px;transition:width .8s cubic-bezier(.4,0,.2,1);position:relative}
.alt-bar-txt{position:absolute;right:3px;top:50%;transform:translateY(-50%);font-size:.48rem;font-weight:600;color:#fff;white-space:nowrap;mix-blend-mode:difference;pointer-events:none}
.alt-count{font-size:.54rem;color:var(--mu);width:20px;text-align:right;flex-shrink:0}
.alt-nome{font-size:.48rem;color:var(--mu);flex:1;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;max-width:88px}
.chips-row{display:flex;flex-wrap:wrap;gap:.22rem;margin-top:.38rem}
.chip{display:inline-flex;align-items:center;gap:.22rem;font-size:.48rem;padding:.13rem .38rem;border-radius:3px;border:1px solid;white-space:nowrap}
.chip.acerto{color:var(--on);border-color:#00ffa330;background:#00ffa308}
.chip.erro{color:var(--off);border-color:#ff3e5e30;background:#ff3e5e08}
.chip.sem{color:var(--mu);border-color:var(--b);background:transparent}
.ri{display:flex;align-items:center;gap:.48rem;padding:.38rem .55rem;background:var(--b);border-radius:7px;margin-bottom:.3rem}
.ri:last-child{margin-bottom:0}
.ri.top1{background:linear-gradient(90deg,#ffd16610,var(--b))}.ri.top2{background:linear-gradient(90deg,#c0c0c00e,var(--b))}.ri.top3{background:linear-gradient(90deg,#cd7f3210,var(--b))}
.rpos{font-family:'DM Serif Display',serif;font-size:.78rem;font-style:italic;width:15px;flex-shrink:0;text-align:center}
.ri.top1 .rpos{color:var(--yw)}.ri.top2 .rpos{color:#c0c0c0}.ri.top3 .rpos{color:#cd7f32}
.rnome{font-size:.6rem;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.rscore{font-family:'DM Serif Display',serif;font-size:.78rem}
.rbar-wrap{width:40px;height:3px;background:var(--bg);border-radius:2px;overflow:hidden;flex-shrink:0}
.rbar{height:100%;border-radius:2px;transition:width .8s}
.obs-dash{font-size:.52rem;color:var(--ac2);border-left:2px solid var(--ac);padding:.3rem .5rem;margin-top:.35rem;line-height:1.5;font-style:italic}
.tempo-row{display:flex;align-items:center;gap:.32rem;font-size:.52rem;padding:.2rem 0;border-bottom:1px solid var(--b2)}
.tempo-row:last-child{border-bottom:none}
.tempo-nome{flex:1;color:var(--mu);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tempo-val{color:var(--yw);font-family:'DM Serif Display',serif;font-size:.7rem}
.vazio{font-size:.6rem;color:var(--mu);text-align:center;padding:.9rem;letter-spacing:.06em}
.voltar{display:block;margin:1rem auto 0;background:transparent;border:1px solid var(--b);color:var(--mu);border-radius:7px;padding:.45rem 1.1rem;font-size:.58rem;cursor:pointer;letter-spacing:.07em;font-family:'IBM Plex Mono',monospace;position:relative;z-index:1;text-transform:uppercase}
.voltar:hover{border-color:var(--mu);color:var(--tx)}
</style></head><body>
<div class="school-bar">
  <div class="school-name">E.E. Ephigenia</div>
  <div class="school-dev">Desenvolvido por Arthur A.</div>
</div>
<div class="topbar">
  <h1>Painel ao vivo</h1>
  <div class="live"><span class="ldot"></span>AO VIVO · <span id="nAl">0</span> aluno(s)</div>
</div>
<div class="metrics">
  <div class="met c-blue"><div class="ml">Alunos</div><div class="mv" id="mAl">0</div><div class="ms">na sala</div></div>
  <div class="met c-on"><div class="ml">Responderam</div><div class="mv" id="mResp">0</div><div class="ms">concluíram</div></div>
  <div class="met c-yw"><div class="ml">Questões</div><div class="mv" id="mQ">0</div><div class="ms">ativas</div></div>
  <div class="met c-ac"><div class="ml">Prova</div><div class="mv" id="mProva" style="font-size:1rem;padding-top:.2rem">—</div><div class="ms" id="mProvaSub">—</div></div>
</div>
<div class="main">
  <div>
    <div class="panel">
      <div class="ptitle">🧠 Questões<span class="pcount" id="qCount"></span></div>
      <div id="qPanel"><p class="vazio">Sem questões.</p></div>
    </div>
  </div>
  <div>
    <div class="panel">
      <div class="ptitle">🏆 Ranking<span class="pcount" id="rankCount"></span></div>
      <div id="rankPanel"><p class="vazio">Sem respostas ainda.</p></div>
    </div>
    <div class="panel">
      <div class="ptitle">⏱ Tempos por questão<span class="pcount" id="tempoCount"></span></div>
      <div id="tempoPanel"><p class="vazio">Sem dados.</p></div>
    </div>
    <div class="panel">
      <div class="ptitle">👥 Presença<span class="pcount" id="presCount"></span></div>
      <div id="presPanel"><p class="vazio">Sem alunos.</p></div>
    </div>
  </div>
</div>
<button class="voltar" onclick="location.href='/admin/painel'">← Voltar ao admin</button>
<script>
var CORES=['linear-gradient(90deg,#ff6b35,#ffb347)','linear-gradient(90deg,#00ffa3,#00d4ff)','linear-gradient(90deg,#ffd166,#ff9500)','linear-gradient(90deg,#ff3e5e,#ff6b35)','linear-gradient(90deg,#4a9eff,#a78bfa)','linear-gradient(90deg,#f72585,#7c6af7)'];
function esc(s){if(!s)return'';var d=document.createElement('div');d.appendChild(document.createTextNode(s));return d.innerHTML}
function pct(a,t){return t?Math.round(a/t*100):0}
function fmtT(ms){if(!ms||ms<=0)return'—';var s=Math.round(ms/1000);if(s<60)return s+'s';return Math.floor(s/60)+'m '+(s%60)+'s';}

function atualizar(d){
  document.getElementById('nAl').textContent=d.cl;
  document.getElementById('mAl').textContent=d.cl;
  document.getElementById('mQ').textContent=d.nQ;
  var resp=0;(d.voters||[]).forEach(function(v){if((v.resp||[]).some(function(r){return r&&r!=='0';}))resp++;});
  document.getElementById('mResp').textContent=resp;
  var ps=d.provaAberta?(d.provaFechada?'FECHADA':'ABERTA'):'AGUARD.';
  var pc=d.provaAberta?(d.provaFechada?'var(--off)':'var(--on)'):'var(--yw)';
  document.getElementById('mProva').textContent=ps;document.getElementById('mProva').style.color=pc;
  document.getElementById('mProvaSub').textContent=d.provaAberta?(d.provaFechada?'encerrada':'recebendo'):'aguardando';

  // Questões
  var qp=document.getElementById('qPanel');
  document.getElementById('qCount').textContent=d.nQ?(d.cl+' aluno(s)'):'';
  if(!d.nQ||!d.qs){qp.innerHTML='<p class="vazio">Sem questões.</p>';}
  else{
    var h='';
    for(var qi=0;qi<d.nQ;qi++){
      var q=d.qs[qi];var gab=q.g&&q.g!=='0'?q.g:'';
      var counts={A:q.cA||0,B:q.cB||0,C:q.cC||0,D:q.cD||0};
      var opts={A:q.a,B:q.b,C:q.c,D:q.d};
      var total=counts.A+counts.B+counts.C+counts.D;
      var acertos=gab?counts[gab]:0;var acc=gab&&total?pct(acertos,total):0;
      var badgeCls=!gab?'cinza':!total?'cinza':acc>=70?'verde':acc>=40?'amarelo':'vermelho';
      var badgeTxt=!gab?'Sem gab':!total?'Aguardando':acc+'% acertos';
      h+='<div class="qblock">';
      h+='<div class="qtopo"><span class="qenun"><b style="color:var(--ac);font-size:.5rem">Q'+(qi+1)+'</b>'+(gab?' <b style="color:var(--on)">['+gab+']</b>':'')+'<br>'+esc(q.t)+'</span><span class="qbadge '+badgeCls+'">'+badgeTxt+'</span></div>';
      h+='<div class="alt-grid">';
      ['A','B','C','D'].forEach(function(l){
        if(!opts[l])return;
        var c=counts[l],w=total?pct(c,total):0;
        var isGab=(l===gab),isErr=gab&&!isGab&&c>0;
        var barColor=isGab?'var(--on)':isErr?'var(--off)':'var(--ac)';
        var ltrColor=isGab?'var(--on)':isErr?'var(--off)':'var(--mu)';
        h+='<div class="alt-row"><span class="alt-ltr" style="color:'+ltrColor+'">'+l+'</span><div class="alt-bar-wrap"><div class="alt-bar" style="width:'+w+'%;background:'+barColor+'">';
        if(w>10)h+='<span class="alt-bar-txt">'+w+'%</span>';
        h+='</div></div><span class="alt-count">'+c+'</span><span class="alt-nome" title="'+esc(opts[l])+'">'+esc(opts[l])+'</span></div>';
      });
      h+='</div>';
      if(d.gabLiberado&&q.obs&&q.obs.trim())h+='<div class="obs-dash">↳ '+esc(q.obs)+'</div>';
      if(d.voters&&d.voters.length){
        var chips='';
        d.voters.forEach(function(v,vi){
          var resp2=v.resp[qi];
          var nm2=esc(v.nome&&v.nome!==v.ip?v.nome:('Aluno '+(vi+1)));
          if(!resp2||resp2==='0')chips+='<span class="chip sem">'+nm2+'</span>';
          else if(gab)chips+='<span class="chip '+(resp2===gab?'acerto':'erro')+'" title="'+resp2+'">'+resp2+' '+nm2+'</span>';
          else chips+='<span class="chip acerto">'+resp2+' '+nm2+'</span>';
        });
        h+='<div class="chips-row">'+chips+'</div>';
      }
      h+='</div>';
    }
    qp.innerHTML=h;
  }

  // Ranking
  var rp=document.getElementById('rankPanel');
  if(!d.voters||!d.voters.length||!d.nQ){rp.innerHTML='<p class="vazio">Sem respostas ainda.</p>';document.getElementById('rankCount').textContent='';return;}
  var temGab=d.gabs&&d.gabs.some(function(g){return g&&g!=='0';});
  var ranked=d.voters.filter(function(v){return(v.resp||[]).some(function(r){return r&&r!=='0';});});
  ranked.sort(function(a,b){
    var sa=0,sb=0;
    if(d.gabs){for(var i=0;i<d.nQ;i++){if(a.resp[i]&&a.resp[i]===d.gabs[i])sa++;if(b.resp[i]&&b.resp[i]===d.gabs[i])sb++;}}
    return sb-sa;
  });
  document.getElementById('rankCount').textContent=ranked.length+' part.';
  if(!ranked.length){rp.innerHTML='<p class="vazio">Sem respostas ainda.</p>';return;}
  var h='';
  ranked.slice(0,10).forEach(function(v,idx){
    var score=0;if(d.gabs)for(var i=0;i<d.nQ;i++){if(v.resp[i]&&v.resp[i]===d.gabs[i])score++;}
    var resp3=(v.resp||[]).filter(function(r){return r&&r!=='0';}).length;
    var cor=temGab?(score===d.nQ?'var(--on)':score>=d.nQ/2?'var(--yw)':'var(--off)'):'var(--blue)';
    var barPct=temGab?pct(score,d.nQ):pct(resp3,d.nQ);
    var clsRi='ri'+(idx===0?' top1':idx===1?' top2':idx===2?' top3':'');
    var scoreTxt=temGab?(score+'/'+d.nQ):(resp3+'/'+d.nQ);
    h+='<div class="'+clsRi+'"><span class="rpos">'+(idx+1)+'</span>';
    h+='<span class="rnome">'+esc(v.nome||v.ip)+'</span>';
    h+='<div class="rbar-wrap"><div class="rbar" style="width:'+barPct+'%;background:'+cor+'"></div></div>';
    h+='<span class="rscore" style="color:'+cor+'">'+scoreTxt+'</span></div>';
  });
  rp.innerHTML=h;

  // Tempos médios por questão
  var tp=document.getElementById('tempoPanel');
  document.getElementById('tempoCount').textContent=(d.nQ||0)+' Q';
  if(!d.nQ||!d.voters||!d.voters.length){tp.innerHTML='<p class="vazio">Sem dados.</p>';}
  else{
    var h='';
    for(var qi=0;qi<d.nQ;qi++){
      var somaT=0,countT=0;
      d.voters.forEach(function(v){if(v.tempos&&v.tempos[qi]>0&&v.tempos[qi]<300000){somaT+=v.tempos[qi];countT++;}});
      var tMed=countT>0?Math.round(somaT/countT):0;
      h+='<div class="tempo-row"><span class="tempo-nome">Q'+(qi+1)+(d.qs&&d.qs[qi]?' · '+esc(d.qs[qi].t.substring(0,28))+'…':'')+'</span><span class="tempo-val">'+fmtT(tMed)+'</span></div>';
    }
    tp.innerHTML=h;
  }

  // Presença
  var pp=document.getElementById('presPanel');
  document.getElementById('presCount').textContent=(d.voters&&d.voters.length)?d.voters.length+'':'';
  if(!d.voters||!d.voters.length){pp.innerHTML='<p class="vazio">Sem alunos.</p>';}
  else{
    var h='';
    d.voters.forEach(function(v,vi){
      var nm=esc(v.nome&&v.nome!==v.ip?v.nome:('Aluno '+(vi+1)));
      var ativo=v.quizIniciado;
      var resp4=(v.resp||[]).filter(function(r){return r&&r!=='0';}).length;
      h+='<div style="display:flex;align-items:center;gap:.38rem;padding:.28rem 0;border-bottom:1px solid var(--b2);font-size:.56rem">';
      h+='<span style="flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+nm+'</span>';
      h+='<span style="font-size:.48rem;padding:.12rem .32rem;border-radius:3px;'+(ativo?'color:var(--on);background:#00ffa310;border:1px solid #00ffa330':'color:var(--mu);background:var(--b);border:1px solid var(--b2)')+'">'+(ativo?'ATIVO':'PRESENTE')+'</span>';
      h+='<span style="color:var(--ac);font-size:.52rem">'+resp4+'/'+d.nQ+'</span>';
      h+='</div>';
    });
    pp.innerHTML=h;
  }
}
function poll(){fetch('/api/dash').then(function(r){return r.json()}).then(atualizar).catch(function(){})}
poll();setInterval(poll,2500);
</script></body></html>)X";

// ════════════════════════════════════════════════════════
//  JSON BUILDERS
// ════════════════════════════════════════════════════════
String jsonQuizAluno(Voter* v){
  if(!v||nQuiz==0){
    String j="{\"qs\":[],\"rs\":[],\"gabLiberado\":false";
    j+=",\"provaAberta\":"  +(String)(provaAberta?"true":"false");
    j+=",\"provaFechada\":" +(String)(provaFechada?"true":"false");
    j+=",\"quizIniciado\":" +(String)(v&&v->quizIniciado?"true":"false");
    j+=",\"euPresente\":"   +(String)(v&&v->presente?"true":"false");
    j+=",\"presentes\":[]}";
     return j;
  }
  String j="{\"qs\":[";
  for(int pos=0;pos<nQuiz;pos++){
    if(pos)j+=",";
    Questao& q=quiz[pos];
    j+="{\"t\":"+je(q.txt)+",\"opts\":[";
    j+=je(q.a)+","+je(q.b);
    if(strlen(q.c))j+=","+je(q.c);
    if(strlen(q.d))j+=","+je(q.d);
    j+="]";
    if(gabLiberado&&q.gab){char gs[2]={q.gab,0};j+=",\"gab\":"+je(gs);}
    if(gabLiberado&&strlen(q.obs))j+=",\"obs\":"+je(q.obs);
    j+="}";
  }
  j += "],\"rs\":[";  // ← ] fecha o array qs, depois vem rs
  for(int pos = 0; pos < nQuiz; pos++){
    if(pos) j += ",";
    if(v->resp[pos] && v->resp[pos] != '\0'){
      char rs[2] = {v->resp[pos], 0};
      j += je(rs);          // "A", "B", "C" ou "D"
    } else {
      j += "\"\"";          // string vazia explícita
    }
  }
  j += "]";
  j+=",\"gabLiberado\":"+(String)(gabLiberado?"true":"false");
  j+=",\"provaAberta\":"+(String)(provaAberta?"true":"false");
  j+=",\"provaFechada\":"+(String)(provaFechada?"true":"false");
  j+=",\"quizIniciado\":"+(String)(v->quizIniciado?"true":"false");
  j+=",\"euPresente\":"+(String)(v->presente?"true":"false");
  j+=",\"presentes\":[";
  bool firstP=true;

  for(int i=0;i<nVoters;i++){
    if(voters[i].presente && voters[i].nome.length()>0){
      if(!firstP)j+=",";
      j+=je(voters[i].nome.c_str());
      firstP=false;
    }
  }
  j+="]}";
  return j;
}

String jsonAdmin(){
  String j="{\"cl\":"+String(nVoters);
  j+=",\"nQ\":"+String(nQuiz);
  j+=",\"gabLiberado\":"+(String)(gabLiberado?"true":"false");
  j+=",\"provaAberta\":"+(String)(provaAberta?"true":"false");
  j+=",\"provaFechada\":"+(String)(provaFechada?"true":"false");
  j+=",\"qs\":[";
  for(int i=0;i<nQuiz;i++){
    if(i)j+=",";Questao& q=quiz[i];char gs[2]={q.gab,0};
    j+="{\"t\":"+je(q.txt)+",\"a\":"+je(q.a)+",\"b\":"+je(q.b)+",\"c\":"+je(q.c)+",\"d\":"+je(q.d)+",\"g\":"+je(gs)+",\"obs\":"+je(q.obs)+"}";
  }
  j+="],\"gabs\":[";
  for(int i=0;i<nQuiz;i++){if(i)j+=",";char gs[2]={quiz[i].gab,0};j+=je(gs);}
  j+="],\"voters\":[";
  for(int i=0;i<nVoters;i++){
    if(i)j+=",";
    j+="{\"nome\":"+je(voters[i].nome.length()?voters[i].nome.c_str():voters[i].ip.c_str());
    j+=",\"ip\":"+je(voters[i].ip.c_str());
    j+=",\"quizIniciado\":"+(String)(voters[i].quizIniciado?"true":"false");
    j+=",\"elegivel\":"+(String)(voters[i].elegivel?"true":"false");
    j+=",\"resp\":[";
    for(int k=0;k<nQuiz;k++){if(k)j+=",";char rs[2]={voters[i].resp[k],0};j+=je(rs);}
    j+="],\"tempos\":[";
    for(int k=0;k<nQuiz;k++){
      if(k)j+=",";
      unsigned long t=voters[i].tRespQ[k];
      j+=String((t>0&&t<300000UL)?t:0);
    }
    j+="]}";
  }
  j+="]}";
  return j;
}

String jsonDash(){
  String j="{\"cl\":"+String(nVoters);
  j+=",\"nQ\":"+String(nQuiz);
  j+=",\"gabLiberado\":"+(String)(gabLiberado?"true":"false");
  j+=",\"provaAberta\":"+(String)(provaAberta?"true":"false");
  j+=",\"provaFechada\":"+(String)(provaFechada?"true":"false");
  j+=",\"qs\":[";
  for(int i=0;i<nQuiz;i++){
    if(i)j+=",";Questao& q=quiz[i];char gs[2]={q.gab,0};
    int cA=0,cB=0,cC=0,cD=0;
    for(int v=0;v<nVoters;v++){char r=voters[v].resp[i];if(r=='A')cA++;else if(r=='B')cB++;else if(r=='C')cC++;else if(r=='D')cD++;}
    j+="{\"t\":"+je(q.txt)+",\"a\":"+je(q.a)+",\"b\":"+je(q.b)+",\"c\":"+je(q.c)+",\"d\":"+je(q.d);
    j+=",\"g\":"+je(gs)+",\"obs\":"+je(q.obs);
    j+=",\"cA\":"+String(cA)+",\"cB\":"+String(cB)+",\"cC\":"+String(cC)+",\"cD\":"+String(cD)+"}";
  }
  j+="],\"gabs\":[";for(int i=0;i<nQuiz;i++){if(i)j+=",";char gs[2]={quiz[i].gab,0};j+=je(gs);}
  j+="],\"voters\":[";
  for(int i=0;i<nVoters;i++){
    if(i)j+=",";
    j+="{\"nome\":"+je(voters[i].nome.length()?voters[i].nome.c_str():voters[i].ip.c_str());
    j+=",\"ip\":"+je(voters[i].ip.c_str());
    j+=",\"quizIniciado\":"+(String)(voters[i].quizIniciado?"true":"false");
    j+=",\"resp\":[";
    for(int k=0;k<nQuiz;k++){if(k)j+=",";char rs[2]={voters[i].resp[k],0};j+=je(rs);}
    j+="],\"tempos\":[";
    for(int k=0;k<nQuiz;k++){
      if(k)j+=",";
      unsigned long t=voters[i].tRespQ[k];
      j+=String((t>0&&t<300000UL)?t:0);
    }
    j+="]}";
  }
  j+="]}";
  return j;
}

String jsonIdx(){
  String j="{\"nQ\":"+String(nQuiz)+",\"nLivros\":"+String(nLivros)+"}";
  return j;
}
String jsonLivros(){
  String j="{\"livros\":[";
  for(int i=0;i<nLivros;i++){
    if(i)j+=",";
    j+="{\"titulo\":"+je(livros[i].titulo)+",\"subtitulo\":"+je(livros[i].subtitulo);
    j+=",\"prefixo\":"+je(livros[i].prefixo)+",\"nPaginas\":"+String(livros[i].nPaginas);
    j+=",\"cor\":"+je(livros[i].cor)+",\"emoji\":"+je(livros[i].emoji)+"}";
  }
  j+="]}";
  return j;
}

// ════════════════════════════════════════════════════════
//  SEND PAGE
// ════════════════════════════════════════════════════════
// Antes: toda página fazia String(FPSTR(...)) + replace() em RAM a cada request —
// isso que causava o engasgo progressivo sob carga (malloc/free de ~9-15KB por
// requisição, fragmentando a SRAM interna até travar). Agora:
//
//  - Páginas que usam %CSS% (Index/Atividades/Biblioteca/Admin): renderizadas
//    UMA VEZ no boot e gravadas no LittleFS. Servidas depois direto do flash,
//    igual já fazia hLivroImg() com as imagens — zero alocação grande por request.
//  - Páginas sem %CSS% (Livro/Dashboard, já tem CSS embutido): servidas direto
//    do PROGMEM via beginResponse_P — nem isso, zero cópia pra RAM.

const char* CACHE_INDEX  = "/_c_index.html";
const char* CACHE_ATIV   = "/_c_ativ.html";
const char* CACHE_BIBLIO = "/_c_biblio.html";
const char* CACHE_ADMIN  = "/_c_admin.html";

void renderECache(const char* pgmStr, const char* destPath){
  String css = String(FPSTR(CSS));
  String pg  = String(FPSTR(pgmStr));
  pg.replace("%CSS%", css);
  File f = LittleFS.open(destPath, "w");
  if(!f){ Serial.printf("[CACHE] ERRO ao abrir %s p/ escrita\n", destPath); return; }
  size_t escrito = f.print(pg);
  f.close();
  Serial.printf("[CACHE] %s gravado (%u bytes)\n", destPath, (unsigned)escrito);
}

// Chamada uma única vez no setup() — recria o cache a cada boot pra nunca
// ficar dessincronizado com o firmware atual (ex: depois de um update OTA).
void prepararCachePaginas(){
  renderECache(PG_INDEX,     CACHE_INDEX);
  renderECache(PG_ATIV,      CACHE_ATIV);
  renderECache(PG_BIBLIOTECA,CACHE_BIBLIO);
  renderECache(PG_ADMIN,     CACHE_ADMIN);
}

void sendCachedPage(AsyncWebServerRequest* request, const char* path){
  if(!LittleFS.exists(path)){ request->send(500,"text/plain","cache ausente: "+String(path)); return; }
  AsyncWebServerResponse* r = request->beginResponse(LittleFS, path, "text/html");
  r->addHeader("Cache-Control","no-store, no-cache, must-revalidate"); // HTML dinâmico: sem cache no navegador
  request->send(r);
}

void sendStaticPage(AsyncWebServerRequest* request, const char* pgmStr){
  AsyncWebServerResponse* r = request->beginResponse_P(200,"text/html",(const uint8_t*)pgmStr, strlen_P(pgmStr));
  r->addHeader("Cache-Control","no-store, no-cache, must-revalidate");
  request->send(r);
}

// ════════════════════════════════════════════════════════
//  ROUTE HANDLERS
// ════════════════════════════════════════════════════════
void hCaptive(AsyncWebServerRequest* req){
  String url  = req->url();
  String host = req->host();

  Serial.printf("[CAPTIVE] %s%s\n", host.c_str(), url.c_str());

  // ── NetworkManager (ArchLinux / Ubuntu) ──────────────────────────────────
  if(url == "/nm-check.txt" || url == "/check_200_public.txt"){
    req->send(200, "text/plain", "NetworkManager is online\n");
    Serial.println("[CAPTIVE] → 200 NetworkManager");
    return;
  }

  // ── iOS / macOS ──────────────────────────────────────────────────────────
  if(url == "/hotspot-detect.html" || url == "/library/test/success.html"){
    req->send(200,"text/html","<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    Serial.println("[CAPTIVE] → 200 Success (iOS)");
    return;
  }

  // ── Windows NCSI ─────────────────────────────────────────────────────────
  if(url == "/ncsi.txt"){
    req->send(200,"text/plain","Microsoft NCSI");
    Serial.println("[CAPTIVE] → 200 NCSI");
    return;
  }
  if(url == "/connecttest.txt"){
    req->send(200,"text/plain","Microsoft Connect Test");
    Serial.println("[CAPTIVE] → 200 connecttest");
    return;
  }

  // ── Qualquer outra URL — 302 para página principal ───────────────────────
  String ip = WiFi.localIP().toString();
  AsyncWebServerResponse* r = req->beginResponse(302);
  r->addHeader("Location", "http://" + ip + "/");
  r->addHeader("Cache-Control", "no-cache");
  req->send(r);
  Serial.printf("[CAPTIVE] → 302 → http://%s/\n", ip.c_str());
}

void hIndex(AsyncWebServerRequest* req)       { sendCachedPage(req, CACHE_INDEX); }
void hAtivPage(AsyncWebServerRequest* req)    { sendCachedPage(req, CACHE_ATIV); }
void hBiblioPage(AsyncWebServerRequest* req)  { sendCachedPage(req, CACHE_BIBLIO); }
void hLivroPage(AsyncWebServerRequest* req)   { if(!req->hasArg("id")){req->redirect("/livros");return;} sendStaticPage(req,PG_LIVRO); }
void hDashPage(AsyncWebServerRequest* req)    { if(!isAdm(req->client()->remoteIP().toString())){req->redirect("/");return;} sendStaticPage(req,PG_DASH); }
void hAdmPainel(AsyncWebServerRequest* req)   { if(!isAdm(req->client()->remoteIP().toString())){req->redirect("/");return;} sendCachedPage(req, CACHE_ADMIN); }

void hLivroImg(AsyncWebServerRequest* req){
  int livroId=0,p=1;
  if(req->hasArg("id"))livroId=req->arg("id").toInt();
  if(req->hasArg("p"))p=req->arg("p").toInt();
  if(livroId<0||livroId>=nLivros){req->send(404,"text/plain","Livro nao encontrado");return;}
  if(p<1)p=1;if(p>livros[livroId].nPaginas)p=livros[livroId].nPaginas;
  char path[32];
  snprintf(path,sizeof(path),"/%s%03d.jpg",livros[livroId].prefixo,p);

  // DEBUG
  Serial.printf("[IMG] livroId=%d p=%d prefixo='%s' path='%s'\n",
    livroId, p, livros[livroId].prefixo, path);
  Serial.printf("[IMG] exists=%d\n", LittleFS.exists(path));

  if(!LittleFS.exists(path)){snprintf(path,sizeof(path),"/%s%d.jpg",livros[livroId].prefixo,p);}
  if(!LittleFS.exists(path)){req->send(404,"text/plain","Pagina nao encontrada: "+String(path));return;}
  AsyncWebServerResponse* r=req->beginResponse(LittleFS,path,"image/jpeg");
  r->addHeader("Cache-Control","public, max-age=86400");
  req->send(r);
}

void hApiNome(AsyncWebServerRequest* req){
  String ip = req->client()->remoteIP().toString();
  Voter* v = getV(ip);

  if(req->hasArg("n")){
    String n = req->arg("n"); n.trim();
    if(n.length() > 0 && n.length() <= 24){
      if(!v){
        // IP desconhecido — tenta registrar pelo nome
        v = registrarPorNome(ip, n);
        if(!v){
          req->send(200,"application/json","{\"ok\":false,\"erro\":\"Nome ja em uso ou sala cheia.\"}");
          return;
        }
      } else {
        v->nome = n;
      }
      marcarSessaoDirty();
    }
  }
  req->send(200,"application/json","{\"ok\":true}");
}


void hApiIdx(AsyncWebServerRequest* req)    { req->send(200,"application/json",jsonIdx()); }
void hApiLivros(AsyncWebServerRequest* req) { req->send(200,"application/json",jsonLivros()); }

void hApiQuiz(AsyncWebServerRequest* req){
  String ip=req->client()->remoteIP().toString();Voter* v=getV(ip);
  if(req->hasArg("n")&&v){String n=req->arg("n");n.trim();if(n.length()>0&&n.length()<=24)v->nome=n;}

  if(req->hasArg("q")&&req->hasArg("r")){
    if(!v){req->send(200,"application/json","{\"erro\":\"Limite atingido.\"}");return;}
    if(!provaAberta||provaFechada){req->send(200,"application/json","{\"erro\":\"Atividade não está aberta.\"}");return;}
    int qi=req->arg("q").toInt();
    if(qi<0||qi>=nQuiz){req->send(200,"application/json","{\"erro\":\"Questão inválida.\"}");return;}
    if(v->resp[qi]!=0){req->send(200,"application/json","{\"erro\":\"Já respondido.\"}");return;}
    char r=req->arg("r")[0];
    if(r!='A'&&r!='B'&&r!='C'&&r!='D'){req->send(200,"application/json","{\"erro\":\"Resposta inválida.\"}");return;}
    v->resp[qi]=r;
    if(req->hasArg("ms")){
      unsigned long ms=req->arg("ms").toInt();
      if(ms>0&&ms<300000UL)v->tRespQ[qi]=ms;
    }
    if(v->tInicioQ[qi]==0) v->tInicioQ[qi]=millis();
    marcarSessaoDirty(); // salva a cada ~10s no loop, não a cada resposta
  }
  req->send(200,"application/json",jsonQuizAluno(v));
}

void hApiQuizEvento(AsyncWebServerRequest* req){
  String ip=req->client()->remoteIP().toString();Voter* v=getV(ip);
  String op=req->hasArg("op")?req->arg("op"):"";
  Serial.printf("[EVENTO] ip=%s op=%s v=%s\n", ip.c_str(), op.c_str(), v?"ok":"null");
  if(v) Serial.printf("[EVENTO] presente=%d quizIniciado=%d\n", (int)v->presente, (int)v->quizIniciado);
  if(req->hasArg("n")&&v){String n=req->arg("n");n.trim();if(n.length()>0&&n.length()<=24)v->nome=n;}
  if(op=="presente"&&v){
    v->presente=true;
    marcarSessaoDirty();
    Serial.printf("[EVENTO] presente setado ip=%s\n", ip.c_str());
  }
  else if(op=="init"&&v){
    v->quizIniciado=true;
    v->presente=true;
    marcarSessaoDirty();
  }
  req->send(200,"application/json",jsonQuizAluno(v));
}

void hApiDash(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"application/json","{\"erro\":\"forbidden\"}");return;}
  req->send(200,"application/json",jsonDash());
}

void hAdmAuth(AsyncWebServerRequest* req){
  String s=req->hasArg("senha")?req->arg("senha"):"";
  String ip=req->client()->remoteIP().toString();
  if(s==ADMIN_PWD){addAdm(ip);req->send(200,"application/json","{\"ok\":true}");}
  else req->send(200,"application/json","{\"ok\":false}");
}
void hAdmData(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  req->send(200,"application/json",jsonAdmin());
}

void hAdmCmd(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  String a=req->hasArg("a")?req->arg("a"):"";
  if(a=="resetQuiz"){resetQuiz();marcarSessaoDirty();salvarQuiz();}
  else if(a=="resetQuizResp"){
    for(int i=0;i<nVoters;i++){
      memset(voters[i].resp,0,MAX_QUEST);
      memset(voters[i].tInicioQ,0,sizeof(voters[i].tInicioQ));
      memset(voters[i].tRespQ,0,sizeof(voters[i].tRespQ));
      voters[i].quizIniciado=false;
    }
    gabLiberado=false;marcarSessaoDirty();
  }
  // No hAdmCmd, adicione:
  else if(a=="limparSessao"){
    nVoters=0;
    nAdm=0;
    resetQuiz();
    marcarSessaoDirty();
  }
  else if(a=="limparQuiz"){nQuiz=0;resetQuiz();salvarQuiz();marcarSessaoDirty();}
  else if(a=="liberarGabarito"){gabLiberado=true;salvarQuiz();}
  else if(a=="ocultarGabarito"){gabLiberado=false;salvarQuiz();}
  else if(a=="abrirProva"){provaAberta=true;provaFechada=false;marcarSessaoDirty();}
  else if(a=="fecharProva"){provaFechada=true;marcarSessaoDirty();}
  req->send(200,"application/json",jsonAdmin());
}

void hAdmAddQ(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(nQuiz>=MAX_QUEST){req->send(200,"application/json","{\"erro\":\"Limite atingido.\"}");return;}
  if(!req->hasArg("e")||!req->hasArg("a")||!req->hasArg("b")){req->send(200,"application/json","{\"erro\":\"Campos obrigatórios.\"}");return;}
  Questao& q=quiz[nQuiz];
  strncpy(q.txt,req->arg("e").c_str(),119);q.txt[119]=0;
  strncpy(q.a,req->arg("a").c_str(),47);q.a[47]=0;
  strncpy(q.b,req->arg("b").c_str(),47);q.b[47]=0;
  strncpy(q.c,req->hasArg("c")?req->arg("c").c_str():"",47);q.c[47]=0;
  strncpy(q.d,req->hasArg("d")?req->arg("d").c_str():"",47);q.d[47]=0;
  String g=req->hasArg("g")?req->arg("g"):"";
  q.gab=(g=="A"||g=="B"||g=="C"||g=="D")?g[0]:0;
  strncpy(q.obs,req->hasArg("obs")?req->arg("obs").c_str():"",159);q.obs[159]=0;
  nQuiz++;
  salvarQuiz();
  req->send(200,"application/json",jsonAdmin());
}

void hAdmDelQ(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  int i=req->hasArg("i")?req->arg("i").toInt():-1;
  if(i<0||i>=nQuiz){req->send(200,"application/json","{\"erro\":\"Índice inválido.\"}");return;}
  for(int j=i;j<nQuiz-1;j++) quiz[j]=quiz[j+1];
  nQuiz--;
  salvarQuiz();marcarSessaoDirty();
  req->send(200,"application/json",jsonAdmin());
}

void printMemInfo() {
  Serial.printf("Heap livre:     %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Heap total:     %u bytes\n", ESP.getHeapSize());
  Serial.printf("Maior bloco:    %u bytes\n", ESP.getMaxAllocHeap());
  Serial.printf("LittleFS used:  %u / %u bytes\n",
    LittleFS.usedBytes(), LittleFS.totalBytes());
  delay(5000);
}

// ── Editar nome do aluno ──
void hAdmEditAluno(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("ip")){req->send(200,"application/json","{\"erro\":\"ip obrigatório\"}");return;}
  String ip=req->arg("ip");
  for(int i=0;i<nVoters;i++){
    if(voters[i].ip==ip){
      if(req->hasArg("nome")){
        String n=req->arg("nome");n.trim();
        if(n.length()>0&&n.length()<=24)voters[i].nome=n;
      }
      marcarSessaoDirty();
      req->send(200,"application/json","{\"ok\":true}");
      return;
    }
  }
  req->send(200,"application/json","{\"erro\":\"aluno não encontrado\"}");
}

void hAdmElegivel(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("ip")){req->send(200,"application/json","{\"erro\":\"ip obrigatorio\"}");return;}
  String ip = req->arg("ip");
  for(int i=0;i<nVoters;i++){
    if(voters[i].ip==ip){
      voters[i].elegivel = true;
      marcarSessaoDirty();
      req->send(200,"application/json",jsonAdmin());
      return;
    }
  }
  req->send(200,"application/json","{\"erro\":\"nao encontrado\"}");
}

// ── Remover aluno da sessão ──
void hAdmDelAluno(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("ip")){req->send(200,"application/json","{\"erro\":\"ip obrigatório\"}");return;}
  String ip=req->arg("ip");
  for(int i=0;i<nVoters;i++){
    if(voters[i].ip==ip){
      for(int j=i;j<nVoters-1;j++) voters[j]=voters[j+1];
      nVoters--;
      marcarSessaoDirty();
      req->send(200,"application/json",jsonAdmin());
      return;
    }
  }
  req->send(200,"application/json","{\"erro\":\"aluno não encontrado\"}");
}

// ── Anular resposta de um aluno em uma questão específica ──
void hAdmAnularResposta(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("ip")||!req->hasArg("q")){req->send(200,"application/json","{\"erro\":\"ip e q obrigatórios\"}");return;}
  String ip=req->arg("ip");
  int qi=req->arg("q").toInt();
  if(qi<0||qi>=nQuiz){req->send(200,"application/json","{\"erro\":\"questão inválida\"}");return;}
  for(int i=0;i<nVoters;i++){
    if(voters[i].ip==ip){
      voters[i].resp[qi]=0;
      voters[i].tRespQ[qi]=0;
      marcarSessaoDirty();
      req->send(200,"application/json",jsonAdmin());
      return;
    }
  }
  req->send(200,"application/json","{\"erro\":\"aluno não encontrado\"}");
}

// ── Nomear atividade atual (antes de arquivar) ──
void hAdmNomeAtividade(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(req->hasArg("nome")){
    String n=req->arg("nome");n.trim();
    if(n.length()>0&&n.length()<=47) strncpy(nomeAtividade,n.c_str(),47);
  }
  req->send(200,"application/json","{\"ok\":true,\"nome\":\""+String(nomeAtividade)+"\"}");
}

// ── Arquivar atividade atual no histórico ──
void hAdmArquivar(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  arquivarAtividade();
  req->send(200,"application/json","{\"ok\":true}");
}

// ── Listar histórico ──
void hAdmHistorico(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  req->send(200,"application/json",jsonListaHist());
}

// ── Retornar dados completos de uma atividade arquivada ──
void hAdmHistData(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("id")){req->send(200,"application/json","{\"erro\":\"id obrigatório\"}");return;}
  int id=req->arg("id").toInt();
  char path[32]; snprintf(path,sizeof(path),"%s/%04d.json",DIR_HIST,id);
  if(!LittleFS.exists(path)){req->send(200,"application/json","{\"erro\":\"não encontrado\"}");return;}
  String raw=fsRead(path);
  req->send(200,"application/json",raw);
}

// ── Deletar atividade arquivada ──
void hAdmHistDel(AsyncWebServerRequest* req){
  if(!isAdm(req->client()->remoteIP().toString())){req->send(403,"text/plain","forbidden");return;}
  if(!req->hasArg("id")){req->send(200,"application/json","{\"erro\":\"id obrigatório\"}");return;}
  int id=req->arg("id").toInt();
  char path[32]; snprintf(path,sizeof(path),"%s/%04d.json",DIR_HIST,id);
  LittleFS.remove(path);
  req->send(200,"application/json",jsonListaHist());
}


// ════════════════════════════════════════════════════════
//  SETUP / LOOP
// ════════════════════════════════════════════════════════
void setup(){
  Serial.begin(BAUD);

  // ── PSRAM: redireciona alocações grandes (JSON dinâmico de /api/dash, /admin/data etc)
  // pra PSRAM em vez da SRAM interna. Isso protege os ~512KB de SRAM interna de
  // fragmentação — foi a fragmentação da SRAM interna que causou a degradação
  // progressiva (0.4s → 1.1s → 3.1s → 7.1s → timeout) no teste de carga.
  // IMPORTANTE: só funciona se a PSRAM estiver habilitada no platformio.ini
  // (board_build.arduino.memory_type). Sem isso, ESP.getPsramSize() retorna 0.
  if(ESP.getPsramSize() > 0){
    heap_caps_malloc_extmem_enable(2048); // alocações > 2KB vão automaticamente pra PSRAM
    Serial.printf("[PSRAM] OK — %u KB totais, %u KB livres (extmem >2KB habilitada)\n",
                  (unsigned)(ESP.getPsramSize()/1024), (unsigned)(ESP.getFreePsram()/1024));
  } else {
    Serial.println("[PSRAM] AVISO: nao detectada! Falta habilitar no platformio.ini:");
    Serial.println("        board_build.arduino.memory_type = qio_opi  (confirme o modelo do seu modulo)");
  }

  pinMode(GPIO_PIN,OUTPUT);pinMode(LED_PIN,OUTPUT);
  digitalWrite(GPIO_PIN,LOW);digitalWrite(LED_PIN,LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_7dBm); // máximo — melhor alcance e estabilidade p/ 15 alunos

  // IP fixo — deve bater com o que está configurado no D-Link como DNS primário
  IPAddress localIP(192, 168, 0, 101);
  IPAddress gateway(192, 168, 0, 254);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns1(192, 168, 0, 254);
  WiFi.config(localIP, gateway, subnet, dns1);

  int tentativas = 0;
  const int MAX_TENTATIVAS = 20;
  WiFi.begin("SI-2.4G", "");

  while(WiFi.status() != WL_CONNECTED && tentativas < MAX_TENTATIVAS){
    delay(1000);
    tentativas++;
    Serial.printf("[WP2] Tentativa %d/%d\n", tentativas, MAX_TENTATIVAS);
  }
  if(WiFi.status() == WL_CONNECTED){
    Serial.println("[WP2] Conectado! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("[WP2] Falhou após todas as tentativas — reiniciando ESP32...");
    delay(2000);
    ESP.restart();
  }

  if(!LittleFS.begin(false)){
    // Primeira tentativa sem formatar — preserva dados
    Serial.println("[LittleFS] Falhou sem format — tentando com format...");
    if(!LittleFS.begin(true)){
      Serial.println("[LittleFS] ERRO CRÍTICO — não foi possível montar!");
    } else {
      Serial.println("[LittleFS] Formatado e montado (dados perdidos — primeira vez?)");
    }
  } else Serial.println("[LittleFS] OK");

  fsEnsureDir(DIR_HIST);  // garante que /hist existe
  prepararCachePaginas(); // pré-renderiza HTML+CSS uma vez no boot, em vez de a cada request

  Serial.println("[STA] Modo: STA puro (servidor)");
  Serial.print("[STA] IP:   "); Serial.println(WiFi.localIP());
  Serial.print("[STA] Canal: "); Serial.println(WiFi.channel());
  Serial.println("[STA] MAC:  " + WiFi.macAddress());

  carregarLivros();
   carregarQuiz();
   carregarSessao();  // ← carrega sessão ANTES de decidir sobre questões padrão
   // Só usa padrão se não há quiz E não há alunos salvos
   if(nQuiz==0 && nVoters==0){
     carregarQuestoesPadrao();
     salvarQuiz();
   }

  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while(f){
    Serial.println(f.name());
    f = root.openNextFile();
  }

  Serial.printf("[Init] %d questões, %d alunos, %d livros\n",nQuiz,nVoters,nLivros);

  server.on("/",                   HTTP_GET, hIndex);
  server.on("/atividades",         HTTP_GET, hAtivPage);
  server.on("/livros",             HTTP_GET, hBiblioPage);
  server.on("/livro/img",          HTTP_GET, hLivroImg);
  server.on("/livro",              HTTP_GET, hLivroPage);
  server.on("/dashboard",          HTTP_GET, hDashPage);
  server.on("/api/nome",           HTTP_GET, hApiNome);
  server.on("/api/idx",            HTTP_GET, hApiIdx);
  server.on("/api/livros",         HTTP_GET, hApiLivros);
  server.on("/api/quiz/evento",    HTTP_GET, hApiQuizEvento);
  server.on("/api/quiz",           HTTP_GET, hApiQuiz);
  server.on("/api/dash",           HTTP_GET, hApiDash);
  server.on("/admin/auth",         HTTP_GET, hAdmAuth);
  server.on("/admin/painel",       HTTP_GET, hAdmPainel);
  server.on("/admin/data",         HTTP_GET, hAdmData);
  server.on("/admin/cmd",          HTTP_GET, hAdmCmd);
  server.on("/admin/addQ",         HTTP_GET, hAdmAddQ);
  server.on("/admin/delQ",         HTTP_GET, hAdmDelQ);
  server.on("/admin/editAluno",     HTTP_GET, hAdmEditAluno);
  server.on("/admin/elegivel",      HTTP_GET, hAdmElegivel);
  server.on("/admin/delAluno",      HTTP_GET, hAdmDelAluno);
  server.on("/admin/anularResposta",HTTP_GET, hAdmAnularResposta);
  server.on("/admin/nomeAtiv",      HTTP_GET, hAdmNomeAtividade);
  server.on("/admin/arquivar",      HTTP_GET, hAdmArquivar);
  server.on("/admin/historico",     HTTP_GET, hAdmHistorico);
  server.on("/admin/histData",      HTTP_GET, hAdmHistData);
  server.on("/admin/histDel",       HTTP_GET, hAdmHistDel);
  // Captive portal — iOS, Windows, Linux
  server.on("/nm-check.txt",             HTTP_GET, hCaptive);  // NetworkManager (Linux)
  server.on("/check_200_public.txt",     HTTP_GET, hCaptive);  // GNOME NetworkManager
  server.on("/connecttest.txt",          HTTP_GET, hCaptive);  // Windows
  server.on("/ncsi.txt",                 HTTP_GET, hCaptive);  // Windows NCSI
  server.on("/hotspot-detect.html",      HTTP_GET, hCaptive);  // iOS / macOS
  server.on("/library/test/success.html",HTTP_GET, hCaptive);  // iOS fallback
  server.on("/redirect",                 HTTP_GET, hCaptive);
  server.onNotFound(hCaptive);

  server.begin();
  Serial.println("[HTTP] Porta 80 OK");

  // DNS server — responde qualquer domínio com o IP do ESP (captive portal via roteador)
  // No D-Link: configurar DNS primário dos clientes como 192.168.0.101
  dnsServer.setTTL(1);   // TTL=1s — MIUI 14 não cacheia, sempre pergunta pro ESP
  dnsServer.start(53, "*", localIP);
  Serial.println("[DNS] Servidor DNS iniciado na porta 53");
  Serial.printf("ip: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Heap livre: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("LittleFS used: %u / %u bytes\n", LittleFS.usedBytes(), LittleFS.totalBytes());
}

void loop(){
  dnsServer.processNextRequest();
  unsigned long agora = millis();
  if(sessaoDirty && (agora - ultimoSalvamento >= SALVAR_INTERVALO_MS)){
    salvarSessao();
    sessaoDirty = false;
    ultimoSalvamento = agora;
  }
}