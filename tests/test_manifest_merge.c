// Unit tests for the AAR manifest merger (ccb_manifest).
#include "test.h"
#include "ccb_manifest.h"
#include "ccb_alloc.h"
#include <stdlib.h>

static const char *APP =
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.app\">\n"
    "    <uses-permission android:name=\"android.permission.INTERNET\" />\n"
    "    <application android:label=\"App\">\n"
    "        <activity android:name=\".MainActivity\" android:exported=\"true\" />\n"
    "    </application>\n"
    "</manifest>\n";

static const char *LIB1 =
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n"
    "    package=\"com.example.lib\">\n"
    "    <uses-permission android:name=\"android.permission.CAMERA\" />\n"
    "    <uses-feature android:name=\"android.hardware.camera\" android:required=\"false\" />\n"
    "    <application>\n"
    "        <activity android:name=\".LibActivity\" android:exported=\"false\" />\n"
    "        <service android:name=\"com.example.lib.LibService\" />\n"
    "        <receiver android:name=\".LibReceiver\">\n"
    "            <intent-filter>\n"
    "                <action android:name=\"com.example.lib.ACTION\" />\n"
    "            </intent-filter>\n"
    "        </receiver>\n"
    "    </application>\n"
    "</manifest>\n";

static char *merge1(const char *app, const char *aar, const char *name,
    ccb_manifest_warnings *warns, char **err) {
  const char *texts[1];
  const char *names[1];
  texts[0] = aar;
  names[0] = name;
  return ccb_manifest_merge(app, texts, names, 1, warns, err);
}

static int contains(const char *hay, const char *needle) {
  return strstr(hay, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle) {
  int n = 0;
  size_t nl = strlen(needle);
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += nl;
  }
  return n;
}

int main(void) {
  // 1. Components + permissions + features merge; relative names resolve.
  {
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, LIB1, "lib1.aar", &warns, &err);
    CHECK(out != NULL);
    CHECK(err == NULL);
    CHECK(warns.count == 0);
    if (out != NULL) {
      CHECK(contains(out, "android.permission.CAMERA"));
      CHECK(contains(out, "android.hardware.camera"));
      CHECK(contains(out, "com.example.lib.LibActivity"));
      CHECK(contains(out, "com.example.lib.LibService"));
      CHECK(contains(out, "com.example.lib.LibReceiver"));
      CHECK(contains(out, "com.example.lib.ACTION"));
      // App entries preserved exactly once.
      CHECK(count_occurrences(out, "android.permission.INTERNET") == 1);
      CHECK(count_occurrences(out, ".MainActivity") == 1);
      // The merged receiver keeps its intent-filter children.
      CHECK(contains(out, "<intent-filter>"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 2. App wins on name conflict, with a warning naming AAR + component.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\"><application>"
        "<activity android:name=\"com.example.app.MainActivity\" android:exported=\"false\" />"
        "<activity android:name=\".Other\" />"
        "</application></manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, lib, "lib2.aar", &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 1);
    if (warns.count == 1) {
      CHECK(contains(warns.msgs[0], "lib2.aar"));
      CHECK(contains(warns.msgs[0], "MainActivity"));
    }
    if (out != NULL) {
      // App's MainActivity kept (exported=true); AAR's copy dropped.
      CHECK(contains(out, ".MainActivity\" android:exported=\"true\""));
      CHECK(count_occurrences(out, "MainActivity") == 1);
      // Non-conflicting component still merged.
      CHECK(contains(out, "com.example.lib.Other"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 3. AAR-vs-AAR identical entries dedup silently.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\"><application>"
        "<service android:name=\".Shared\" />"
        "</application></manifest>";
    const char *texts[2];
    const char *names[2];
    texts[0] = lib;
    texts[1] = lib;
    names[0] = "a.aar";
    names[1] = "b.aar";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = ccb_manifest_merge(APP, texts, names, 2, &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 0);
    if (out != NULL) {
      CHECK(count_occurrences(out, "com.example.lib.Shared") == 1);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 4. AAR-vs-AAR same name, different attributes: first wins + warning.
  {
    const char *lib_a =
        "<manifest package=\"com.example.lib\"><application>"
        "<service android:name=\".Shared\" android:exported=\"true\" />"
        "</application></manifest>";
    const char *lib_b =
        "<manifest package=\"com.example.lib\"><application>"
        "<service android:name=\".Shared\" android:exported=\"false\" />"
        "</application></manifest>";
    const char *texts[2];
    const char *names[2];
    texts[0] = lib_a;
    texts[1] = lib_b;
    names[0] = "a.aar";
    names[1] = "b.aar";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = ccb_manifest_merge(APP, texts, names, 2, &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 1);
    if (warns.count == 1) {
      CHECK(contains(warns.msgs[0], "b.aar"));
      CHECK(contains(warns.msgs[0], "a.aar"));
      CHECK(contains(warns.msgs[0], "Shared"));
    }
    if (out != NULL) {
      CHECK(contains(out, "android:exported=\"true\""));
      CHECK(count_occurrences(out, "Shared") == 1);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 5. Malformed AAR manifest: hard error naming the AAR.
  {
    const char *bad =
        "<manifest package=\"com.example.lib\"><application>"
        "<activity android:name=\".X\"></application></manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, bad, "bad.aar", &warns, &err);
    CHECK(out == NULL);
    CHECK(err != NULL);
    if (err != NULL) {
      CHECK(contains(err, "bad.aar"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 6. Manifest-less AAR: merged as nothing, no warning, app unchanged.
  {
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, NULL, "nomanifest.aar", &warns, &err);
    CHECK(out != NULL);
    CHECK(err == NULL);
    CHECK(warns.count == 0);
    if (out != NULL) {
      CHECK_STR_EQ(out, APP);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 7. Relative names resolve per-manifest, so cross-AAR duplicates match.
  {
    const char *lib_a =
        "<manifest package=\"com.example.lib\"><application>"
        "<service android:name=\".Shared\" />"
        "</application></manifest>";
    const char *lib_b =
        "<manifest package=\"com.example.other\"><application>"
        "<service android:name=\"com.example.lib.Shared\" />"
        "</application></manifest>";
    const char *texts[2];
    const char *names[2];
    texts[0] = lib_a;
    texts[1] = lib_b;
    names[0] = "a.aar";
    names[1] = "b.aar";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = ccb_manifest_merge(APP, texts, names, 2, &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 0);
    if (out != NULL) {
      CHECK(count_occurrences(out, "com.example.lib.Shared") == 1);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 8. Malformed app manifest: error, no crash.
  {
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1("<manifest><application>", LIB1, "lib1.aar", &warns, &err);
    CHECK(out == NULL);
    CHECK(err != NULL);
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 9. Nameless uses-feature (glEsVersion) dedups by attributes.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\">"
        "<uses-feature android:glEsVersion=\"0x00030000\" />"
        "</manifest>";
    const char *texts[2];
    const char *names[2];
    texts[0] = lib;
    texts[1] = lib;
    names[0] = "a.aar";
    names[1] = "b.aar";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = ccb_manifest_merge(APP, texts, names, 2, &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 0);
    if (out != NULL) {
      CHECK(count_occurrences(out, "glEsVersion") == 1);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 10. Component without android:name is a malformed-manifest error.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\"><application>"
        "<activity android:exported=\"true\" />"
        "</application></manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, lib, "noname.aar", &warns, &err);
    CHECK(out == NULL);
    CHECK(err != NULL);
    if (err != NULL) {
      CHECK(contains(err, "noname.aar"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 11. Application attributes and meta-data from AARs are NOT merged.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\"><application android:theme=\"@style/LibTheme\">"
        "<meta-data android:name=\"lib.key\" android:value=\"v\" />"
        "<provider android:name=\".LibProvider\" android:authorities=\"com.example.lib.p\" />"
        "<activity-alias android:name=\".Alias\" android:targetActivity=\".MainActivity\" />"
        "</application></manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, lib, "lib3.aar", &warns, &err);
    CHECK(out != NULL);
    if (out != NULL) {
      CHECK(!contains(out, "LibTheme"));
      CHECK(!contains(out, "lib.key"));
      CHECK(contains(out, "com.example.lib.LibProvider"));
      CHECK(contains(out, "com.example.lib.Alias"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 12. App without <application>: components cannot merge, uses-* can.
  {
    const char *noapp =
        "<manifest package=\"com.example.app\">"
        "</manifest>";
    const char *lib_comp =
        "<manifest package=\"com.example.lib\"><application>"
        "<service android:name=\".S\" />"
        "</application></manifest>";
    const char *lib_perm =
        "<manifest package=\"com.example.lib\">"
        "<uses-permission android:name=\"android.permission.CAMERA\" />"
        "</manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(noapp, lib_comp, "c.aar", &warns, &err);
    CHECK(out == NULL);
    CHECK(err != NULL);
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
    memset(&warns, 0, sizeof(warns));
    err = NULL;
    out = merge1(noapp, lib_perm, "p.aar", &warns, &err);
    CHECK(out != NULL);
    if (out != NULL) {
      CHECK(contains(out, "android.permission.CAMERA"));
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 13. Zero AARs returns the app text unchanged.
  {
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = ccb_manifest_merge(APP, NULL, NULL, 0, &warns, &err);
    CHECK(out != NULL);
    if (out != NULL) {
      CHECK_STR_EQ(out, APP);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  // 14. Uses-permission conflict with the app warns and keeps app's copy.
  {
    const char *lib =
        "<manifest package=\"com.example.lib\">"
        "<uses-permission android:name=\"android.permission.INTERNET\" />"
        "</manifest>";
    ccb_manifest_warnings warns;
    memset(&warns, 0, sizeof(warns));
    char *err = NULL;
    char *out = merge1(APP, lib, "p.aar", &warns, &err);
    CHECK(out != NULL);
    CHECK(warns.count == 1);
    if (warns.count == 1) {
      CHECK(contains(warns.msgs[0], "p.aar"));
      CHECK(contains(warns.msgs[0], "INTERNET"));
    }
    if (out != NULL) {
      CHECK(count_occurrences(out, "android.permission.INTERNET") == 1);
    }
    free(out);
    free(err);
    ccb_manifest_warnings_free(&warns);
  }
  TEST_MAIN_END();
}
