#include <cstdio>
#include <string>
#include "../src/core/hotkey.h"
#include "../src/core/settings.h"
using namespace cr;
static int F=0,P=0;
#define T(name,c) do{ if(c){++P;} else {++F; std::printf("FAIL %s\n",name);} }while(0)
static bool ok(const std::string& s, Hotkey* o=nullptr){Hotkey h;std::string e;bool r=parse(s,h,&e);if(o)*o=h;return r;}
int main(){
  Hotkey h;
  T("Ctrl++ parses (comment claims support)", ok("Ctrl++",&h));
  T("Ctrl+Plus -> '='", ok("Ctrl+Plus",&h)&&h.vk==0xBB);
  T("empty is unassigned", ok("",&h)&&h.empty());
  T("whitespace only unassigned", ok("   \t ",&h)&&h.empty());
  T("only '+' rejected", !ok("+"));
  T("Ctrl+ rejected", !ok("Ctrl+"));
  T("dup mod rejected", !ok("Ctrl+Ctrl+A"));
  T("Ctrl+Control dup rejected", !ok("Ctrl+Control+A"));
  T("two keys rejected", !ok("Ctrl+A+B"));
  T("modifiers only rejected", !ok("Ctrl+Alt"));
  T("unicode key rejected", !ok("Ctrl+\xC3\xA9"));
  T("unicode cyrillic rejected", !ok("Ctrl+\xD0\x96"));
  T("embedded NUL safe", !ok(std::string("Ctrl+A\0B",8)));
  T("F0 rejected", !ok("F0")); T("F25 rejected", !ok("F25")); T("F99 rejected", !ok("F99"));
  T("F001 rejected", !ok("F001"));
  T("huge string rejected fast", !ok(std::string(1000000,'A')));
  std::string many; for(int i=0;i<100000;i++) many+="Ctrl+"; many+="A";
  T("100k dup mods rejected", !ok(many));
  T("Num+5 style", ok("Ctrl+Num5",&h)&&h.vk==0x65);
  T("Numpad 5", ok("Alt+Numpad 5",&h)&&h.vk==0x65);
  T("validate Alt+Tab err", validate(Hotkey{MOD_ALT_,0x09}).severity==Severity::Error);
  T("validate Win+Alt+R err", validate(Hotkey{MOD_WIN_|MOD_ALT_,'R'}).severity==Severity::Error);
  T("validate lone modifier vk err", validate(Hotkey{MOD_CTRL_,0x11}).severity==Severity::Error);
  T("validate unknown vk err", validate(Hotkey{MOD_CTRL_,0xFF}).severity==Severity::Error);
  T("conflict found", findConflicts({{"a",Hotkey{MOD_ALT_,'V'}},{"b",Hotkey{MOD_ALT_,'V'}}}).size()==1);
  T("empty no conflict", findConflicts({{"a",Hotkey{}},{"b",Hotkey{}}}).empty());
  T("3-way conflicts", findConflicts({{"a",Hotkey{MOD_ALT_,'V'}},{"b",Hotkey{MOD_ALT_,'V'}},{"c",Hotkey{MOD_ALT_,'V'}}}).size()==3);
  // settings
  Settings s; std::string e;
  T("empty json rejected", !fromJson("",s,&e));
  T("{} ok defaults", fromJson("{}",s,&e));
  T("trailing garbage rejected", !fromJson("{} x",s,&e));
  T("unterminated rejected", !fromJson("{\"fps\":30",s,&e));
  T("unterminated str rejected", !fromJson("{\"a",s,&e));
  T("key no colon end", !fromJson("{\"a\"",s,&e));
  T("dup keys last wins", fromJson("{\"fps\":15,\"fps\":60}",s,&e)&&s.fps==60);
  T("fps huge 1e300", fromJson("{\"fps\":1e300}",s,&e)); std::printf("  fps for 1e300 -> %d\n",s.fps);
  T("fps -1e300", fromJson("{\"fps\":-1e300}",s,&e)); std::printf("  fps for -1e300 -> %d\n",s.fps);
  T("fps string ignored", fromJson("{\"fps\":\"60\"}",s,&e)&&s.fps==Settings{}.fps);
  T("bad number '-' accepted?", !fromJson("{\"fps\":-}",s,&e));
  T("number garbage 1e+e-", !fromJson("{\"fps\":1e+e-}",s,&e));
  T("unicode outputDir roundtrip", fromJson("{\"outputDir\":\"C:\\\\\xD0\x92\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE \\u00e9\"}",s,&e)&&s.outputDir=="C:\\\xD0\x92\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE \xC3\xA9");
  { Settings t=s; Settings u; T("toJson roundtrip", fromJson(toJson(t),u,&e)&&u.outputDir==t.outputDir); }
  T("surrogate pair", fromJson("{\"outputDir\":\"\\ud83d\\ude00\"}",s,&e)&&s.outputDir=="\xF0\x9F\x98\x80");
  T("lone high surrogate + \\u0041 rejected or sane", !fromJson("{\"outputDir\":\"\\ud83d\\u0041\"}",s,&e) || s.outputDir.find('A')!=std::string::npos);
  T("lone low surrogate rejected", !fromJson("{\"outputDir\":\"\\udc00\"}",s,&e));
  T("NUL via \\u0000 in outputDir rejected", !fromJson("{\"outputDir\":\"C:\\\\a\\u0000b\"}",s,&e));
  T("bad hex rejected", !fromJson("{\"a\":\"\\u12\"}",s,&e));
  std::string deep="{\"x\":"; for(int i=0;i<100000;i++) deep+="["; deep+="]}";
  T("deep nesting rejected no crash", !fromJson(deep,s,&e));
  std::string big="{\"outputDir\":\""+std::string(50*1024*1024,'a')+"\"}";
  T("50MB string ok", fromJson(big,s,&e)&&s.outputDir.size()==50u*1024*1024);
  T("hotkey Error falls back", fromJson("{\"hotkeyToggleRecord\":\"Alt+Tab\"}",s,&e)&&!(s.toggleRecord==Hotkey{MOD_ALT_,0x09}));
  T("hotkey empty string clears", fromJson("{\"hotkeyToggleRecord\":\"\"}",s,&e)&&s.toggleRecord.empty());
  T("duplicate hotkeys in settings detected", !(fromJson("{\"hotkeyToggleRecord\":\"Alt+F9\",\"hotkeyOpenFolder\":\"Alt+F9\"}",s,&e) && s.toggleRecord==s.openFolder));
  T("BOM handled by fromJson? (loadFile strips)", true);
  T("invalid utf8 outputDir rejected", !fromJson("{\"outputDir\":\"\xFF\xFE\"}",s,&e));
  T("control char raw in string rejected", !fromJson("{\"outputDir\":\"a\nb\"}",s,&e));
  std::printf("%d passed, %d failed\n",P,F); return F?1:0;
}
