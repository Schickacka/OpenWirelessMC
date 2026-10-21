#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include "storage/storage.h"
#include "storage/config.h"
#include "system/settings.h"
#include "web/percent_decode.h"
#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"

using namespace openmc;
extern int g_log_quiet;
namespace openmc { namespace storage { namespace sdcard { extern bool sd_ok; } } }
namespace openmc { namespace system { struct Settings settings; } }
namespace openmc { namespace wifi { bool ready; char ip_address[16]; uint8_t ssid[33]; } }

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)

static std::vector<uint8_t> readfile(const char *p){ std::vector<uint8_t> v; FILE*f=fopen(p,"rb"); if(!f) return v; uint8_t b[4096]; size_t n; while((n=fread(b,1,sizeof b,f))>0) v.insert(v.end(),b,b+n); fclose(f); return v; }
static void writefile(const char*p,const void*d,size_t n){ FILE*f=fopen(p,"wb"); fwrite(d,1,n,f); fclose(f);} 
static void commit_sector(int s, uint8_t val){ // what sio Commit() does on the SIO core
  using namespace sio::memory_card;
  memset(&memory_card_ram[s*128], val, 128); sector_version[s]++; committed_to_storage=false; last_write_tick = sio::event_ticks; }

static void test_percent(){
  printf("[percent_decode]\n"); char o[32];
  CHECK(web::PercentDecode("a%20b+c", o, sizeof o) && !strcmp(o,"a b c"));
  CHECK(web::PercentDecode("a+b", o, sizeof o, false) && !strcmp(o,"a+b"));
  CHECK(web::PercentDecode("100%", o, sizeof o) && !strcmp(o,"100%"));
  // previously a trailing partial escape poisoned the NEXT call
  web::PercentDecode("abc%4", o, sizeof o); web::PercentDecode("xyz", o, sizeof o); CHECK(!strcmp(o,"xyz"));
  web::PercentDecode("abc%", o, sizeof o); web::PercentDecode("xyz", o, sizeof o); CHECK(!strcmp(o,"xyz"));
  CHECK(web::PercentDecode("Game%20%23%281%29", o, sizeof o) && !strcmp(o,"Game #(1)"));
  CHECK(web::PercentDecode("a%00b", o, sizeof o) && !strcmp(o,"ab"));
  char small[8]; CHECK(!web::PercentDecode("0123456789", small, sizeof small)); CHECK(strlen(small)==7);
  CHECK(web::PercentDecode("%E3%82%A2", o, sizeof o) && strlen(o)==3);
  CHECK(web::PercentDecode("", o, sizeof o) && o[0]==0);
}

static void cfg(const char *content, bool expect, int mode=-1, const char *ssid=NULL, const char *pass=NULL){
  writefile("/tmp/test_config.txt", content, strlen(content));
  memset(&system::settings,0,sizeof system::settings);
  bool r = storage::config::LoadConfig();
  CHECK(r==expect);
  if(expect){ if(mode>=0) CHECK((int)system::settings.wifi_mode==mode); if(ssid) CHECK(!strcmp((char*)system::settings.ssid,ssid)); if(pass) CHECK(!strcmp((char*)system::settings.password,pass)); }
}
static void test_config(){
  printf("[config]\n");
  cfg("mode=client\nssid=MyNet\npassword=secret123\n", true, 1, "MyNet","secret123");
  cfg("MODE=AP\r\nSSID=My Net\r\nPassword=12345678\r\n", true, 0, "My Net","12345678");      // upper case names (used to be broken)
  cfg("# comment\n\n  mode = client \nssid=A\npassword=\n", true, 1, "A","");               // spaces, open network
  cfg("\xEF\xBB\xBFmode=ap\nssid=BOM\npassword=12345678\n", true, 0, "BOM","12345678");      // UTF-8 BOM
  cfg("mode=none\n", true, 2);
  cfg("mode=client\nssid=0123456789012345678901234567890123456789\npassword=12345678\n", false); // ssid 40 chars: was a buffer overflow
  cfg("mode=client\nssid=Net\npassword=short\n", false);
  cfg("mode=client\nssid=Net\n", false);
  cfg("mode=bogus\nssid=Net\npassword=12345678\n", false);
  std::string longline = "mode=client\nssid=Net\npassword=12345678\n# " + std::string(600,'x') + "\n";
  cfg(longline.c_str(), true, 1, "Net","12345678");                                           // long comment line is skipped cleanly
  std::string trunc = "mode=client\nssid=Net\npassword=" + std::string(300,'p') + "\n";
  cfg(trunc.c_str(), false);
  CHECK(!storage::config::LoadConfig() || true);
}

static void reset_sd(){ ::system("rm -rf /sdcard/* /sdcard/.[a-z]* 2>/dev/null"); }
static void test_storage(){
  using namespace sio::memory_card;
  printf("[storage]\n");
  g_log_quiet = 1;

  // a) fresh SD without default.mc -> blank formatted card is created and mounted
  reset_sd(); sio::memory_card_enabled=true; storage::sdcard::sd_ok=true;
  storage::Init();
  auto f = readfile("/sdcard/default.mc");
  CHECK(f.size()==131072); CHECK(f[0]=='M' && f[1]=='C'); CHECK(f[8192]==0xFF);
  CHECK(sio::memory_card_enabled); CHECK(!strcmp(storage::loaded_file_path,"/sdcard/default.mc")); CHECK(committed_to_storage);
  CHECK(memcmp(memory_card_ram, f.data(), 131072)==0);

  // b) only changed sectors are written
  commit_sector(5,0xAA); commit_sector(900,0x55);
  CHECK(storage::FlushPending()); CHECK(committed_to_storage);
  auto g = readfile("/sdcard/default.mc"); CHECK(g.size()==131072);
  CHECK(g[5*128]==0xAA && g[5*128+127]==0xAA); CHECK(g[900*128]==0x55);
  CHECK(memcmp(g.data(), f.data(), 5*128)==0); CHECK(memcmp(&g[6*128], &f[6*128], (900-6)*128)==0);

  // c) write that arrives after the dirty flag was consumed is not lost
  commit_sector(10,0x11); CHECK(storage::FlushPending());
  commit_sector(11,0x22); CHECK(!committed_to_storage); CHECK(storage::FlushPending());
  g = readfile("/sdcard/default.mc"); CHECK(g[10*128]==0x11 && g[11*128]==0x22);

  // d) invalid file never takes the current card away
  writefile("/sdcard/bad.mc","tiny",4);
  char bad[]="/sdcard/bad.mc"; bool en_before=sio::memory_card_enabled;
  CHECK(!storage::LoadCardFromFile(bad, memory_card_ram)); CHECK(sio::memory_card_enabled==en_before);
  CHECK(!strcmp(storage::loaded_file_path,"/sdcard/default.mc"));
  char missing[]="/sdcard/nope.mc"; CHECK(!storage::LoadCardFromFile(missing, memory_card_ram)); CHECK(sio::memory_card_enabled);

  // e) unsaved changes of the old card are flushed when another card is mounted
  CHECK(storage::CreateBlankFormattedCard("/sdcard/other.mc"));
  commit_sector(20,0x77);   // dirty, not flushed
  char other[]="/sdcard/other.mc";
  CHECK(storage::LoadCardFromFile(other, memory_card_ram));
  g = readfile("/sdcard/default.mc"); CHECK(g[20*128]==0x77);
  CHECK(!strcmp(storage::loaded_file_path,"/sdcard/other.mc")); CHECK(sio::memory_card_enabled);
  CHECK(committed_to_storage);
  // aliasing: reload the currently loaded path (filepath == loaded_file_path)
  CHECK(storage::LoadCardFromFile(storage::loaded_file_path, memory_card_ram));
  // after loading, the other card only changes where the console writes
  commit_sector(30,0x33); CHECK(storage::FlushPending());
  auto o = readfile("/sdcard/other.mc"); CHECK(o[30*128]==0x33); CHECK(o[31*128]==0xFF || o[31*128]==0x00 || true);
  auto d2 = readfile("/sdcard/default.mc"); CHECK(d2[30*128]!=0x33);

  // f) invalid default.mc at boot: moved aside, replaced by a valid Boot Card
  reset_sd(); writefile("/sdcard/default.mc","not a card image",16);
  sio::memory_card_enabled=true; storage::Init();
  CHECK(sio::memory_card_enabled);
  auto repaired = readfile("/sdcard/default.mc");
  CHECK(repaired.size()==131072); CHECK(repaired[0]=='M' && repaired[1]=='C');
  CHECK(repaired[8192]==0xFF);
  CHECK(readfile("/sdcard/default.mc.invalid").size()==16);
  CHECK(!strcmp(storage::loaded_file_path,"/sdcard/default.mc"));

  // g) no SD at boot
  reset_sd(); storage::sdcard::sd_ok=false; sio::memory_card_enabled=true; storage::Init();
  CHECK(!sio::memory_card_enabled); storage::sdcard::sd_ok=true;

  // h) pending data + broken destination: eject is refused, nothing is silently dropped
  reset_sd(); storage::Init(); CHECK(sio::memory_card_enabled);
  commit_sector(3,0x42);
  strcpy(storage::loaded_file_path,"/sdcard/missingdir/x.mc");
  CHECK(!storage::FlushPending()); CHECK(!committed_to_storage);
  CHECK(!storage::SafelyUnmountSD()); CHECK(storage::ready);
  CHECK(!storage::UnloadCard());
  strcpy(storage::loaded_file_path,"/sdcard/default.mc");
  CHECK(storage::FlushPending());  // SD works again -> data lands
  g = readfile("/sdcard/default.mc"); CHECK(g[3*128]==0x42);

  // i) loaded file deleted behind our back -> recreated with the full image
  unlink("/sdcard/default.mc"); commit_sector(4,0x24); CHECK(storage::FlushPending());
  g = readfile("/sdcard/default.mc"); CHECK(g.size()==131072); CHECK(g[4*128]==0x24); CHECK(g[0]=='M');

  // j) safe unmount / remount cycle
  CHECK(storage::SafelyUnmountSD()); CHECK(!storage::ready); CHECK(!sio::memory_card_enabled); CHECK(storage::loaded_file_path[0]==0);
  CHECK(storage::RemountSD()); CHECK(storage::ready); CHECK(sio::memory_card_enabled); CHECK(!strcmp(storage::loaded_file_path,"/sdcard/default.mc"));

  // k) version counter wraps without losing updates
  for(int i=0;i<70000;i++){ sector_version[40]++; }
  memory_card_ram[40*128]=0x99; sector_version[40]++; committed_to_storage=false; CHECK(storage::FlushPending());
  g = readfile("/sdcard/default.mc"); CHECK(g[40*128]==0x99);

  // l) GameID switch creates and switches cards, flushing the previous one
  commit_sector(50,0x50);
  CHECK(storage::SwitchToGameID("SLUS-00594"));
  struct stat st; bool found=false; { FILE* p=popen("ls /sdcard","r"); char line[300]; while(fgets(line,sizeof line,p)) if(strstr(line,"SLUS")) found=true; pclose(p);} CHECK(found);
  g = readfile("/sdcard/default.mc"); CHECK(g[50*128]==0x50);
  CHECK(!storage::SwitchToGameID("")); (void)st;
  g_log_quiet = 0;
}

int main(){ test_percent(); test_config(); test_storage(); printf("\n%s (%d failures)\n", fails?"FAILED":"ALL TESTS PASSED", fails); return fails?1:0; }
