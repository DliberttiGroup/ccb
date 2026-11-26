// ccb_dexcheck: post-build dex check (change-dexcheck).
//
// Parses the dex files d8/R8 produced and reports classes the dex
// references but does not define. A missing program class (the
// kotlin-stdlib Intrinsics crash, the RecyclerView R$attr crash)
// is a build error here, before packaging, instead of a crash on
// the phone. Platform classes (java.*, android.*, ...) come from
// the --lib inputs and are never in the dex; they are allowlisted,
// never reported.
//
// Missing names are dotted class names (e.g.
// "kotlin.jvm.internal.Intrinsics"), sorted, deduplicated.
// Reflection-only references (Class.forName strings) cannot be
// caught: only types the dex actually references are checked.
#ifndef CCB_DEXCHECK_H
#define CCB_DEXCHECK_H

#include "ccb_common.h"

// Parse one dex file. On success returns CCB_OK with *missing_out
// set (NULL + *nmissing_out 0 when clean; caller frees with
// ccb_free_str_array). On a malformed/truncated file returns
// CCB_ERR_BUILD with *err_out set to a malloc'd message.
ccb_status ccb_dexcheck_file(const char *path, char ***missing_out,
    size_t *nmissing_out, char **err_out);

// Same, over every *.dex file under dir (sorted order). No dex
// files at all is CCB_ERR_BUILD ("no dex files found").
ccb_status ccb_dexcheck_dir(const char *dir, char ***missing_out,
    size_t *nmissing_out, char **err_out);

#endif
