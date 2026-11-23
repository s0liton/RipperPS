// Which language folders a PS5 dump leaves out (src/service/langs.c).
#include <string.h>

#include "langs.h"
#include "ripper/common.h"
#include "test.h"

void test_langs(void) {
  // Deathloop (PPSA01302), as installed from a US disc.
  static const char scenario[] =
      "{\"chunkDefaultLanguage\":\"en-US\",\"chunkSupportedLanguages\":[\"en-US\",\"fr-FR\",\"pt-BR\",\"es-419\"],"
      "\"scenarioCount\":1}";
  struct langs l;

  CHECK(langs_from_scenario(scenario, &l) == RIP_OK);
  CHECK(l.n == 4);
  CHECK(langs_has(&l, "en") && langs_has(&l, "fr") && langs_has(&l, "pt-BR") && langs_has(&l, "es-419"));
  CHECK(!langs_has(&l, "es") && !langs_has(&l, "pt") && !langs_has(&l, "de"));
  CHECK(l.main && !strcmp(l.main, "en"));

  // Its folders: base/eLID_<Name> and base/audio/banks/elid_<name>.
  CHECK_STR(langs_folder("eLID_English"), "en");
  CHECK_STR(langs_folder("eLID_BrazilianPortuguese"), "pt-BR");
  CHECK_STR(langs_folder("elid_mexicanspanish"), "es-419");
  CHECK_STR(langs_folder("eLID_Spanish"), "es");
  CHECK_STR(langs_folder("elid_german"), "de");
  CHECK_STR(langs_folder("eLID_Japanese"), "ja");
  // Other ways games name them.
  CHECK_STR(langs_folder("lang_ptBR"), "pt-BR");
  CHECK_STR(langs_folder("de-DE"), "de");
  CHECK_STR(langs_folder("Russian"), "ru");
  CHECK_STR(langs_folder("zh_Hans"), "zh");
  // Not languages: kept.
  CHECK(langs_folder("audio") == NULL);
  CHECK(langs_folder("banks") == NULL);
  CHECK(langs_folder("base") == NULL);
  CHECK(langs_folder("sce_sys") == NULL);
  CHECK(langs_folder("it") == NULL);
  CHECK(langs_folder("lid") == NULL);

  CHECK_STR(langs_name("es-419"), "Latin American Spanish");

  // playgo_stub.dat's language line: Deathloop's four, then English alone.
  {
    char names[200];
    struct langs one;
    struct langs none;
    CHECK(langs_playgo(&l, names, sizeof(names)) == 4);
    CHECK_STR(names, "english_us,english_gb,french,french_ca,portuguese_br,spanish_la");
    memset(&one, 0, sizeof(one));
    one.keys[one.n++] = "en";
    CHECK(langs_playgo(&one, names, sizeof(names)) == 1);
    CHECK_STR(names, "english_us,english_gb");
    // Ukrainian has no name there: nothing to write.
    memset(&none, 0, sizeof(none));
    none.keys[none.n++] = "uk";
    CHECK(langs_playgo(&none, names, sizeof(names)) == 0);
    CHECK_STR(names, "");
    // Too small for a language: left out, never cut short.
    CHECK(langs_playgo(&one, names, 10) == 0);
    CHECK_STR(names, "");
  }

  CHECK(langs_from_scenario("{}", &l) == RIP_ENOENT);
  CHECK(langs_from_scenario("not json", &l) == RIP_ENOENT);
  CHECK(langs_from_scenario(NULL, &l) == RIP_ENOENT);
}
