// ccb_preview: terminal UI preview for CCB UI-convention projects.
// A linter with a picture, NOT an emulator: it statically parses a CSS
// subset and Kotlin flow structure, lays boxes out on a phone frame in
// terminal columns, and renders wireframes + a logic tree + diagnostics.
// It never executes Kotlin, never evaluates conditions, never fetches
// data, never animates. Everything outside the documented subset warns
// and falls back; unknown constructs become labeled unknown nodes.
//
// Ownership: every preview allocation lives in one arena owned by the
// project (or passed in by unit tests). Freeing the project destroys
// the arena. The diagnostic list is the one exception: it owns malloc'd
// strings and is freed with ccb_diag_list_free by pv_project_free.
#ifndef CCB_PREVIEW_H
#define CCB_PREVIEW_H

#include "ccb_common.h"
#include "ccb_alloc.h"
#include "ccb_str.h"
#include "ccb_diag.h"
#include "ccb_config.h"

// Diagnostic codes (see docs/diagnostics.md).
// CCB-P1001 unknown CSS property (warning; error under --strict)
// CCB-P1002 CSS parse error, partial parse kept (warning)
// CCB-P1003 overlapping boxes (warning)
// CCB-P1004 off-screen element (warning)
// CCB-P1005 navigation to an unknown screen (error)
// CCB-P1006 unreachable screen (warning)
// CCB-P1007 empty screen (warning)
// CCB-P1008 duplicate screen (error)
// CCB-P1009 text clipped to its box (warning)
// CCB-P1010 flow parse error, partial parse kept (warning)
// CCB-P1011 no screens found (warning)
// CCB-P1012 unknown Kotlin construct, kept as a generic box (warning)
// CCB-P1013 frame width clamped to the minimum (warning)
// CCB-P1014 CSS value outside the subset, e.g. unsupported unit or
//   display value (warning; error under --strict)
// CCB-P1015 CSS file ignored: it is neither common.css nor a
//   <Screen>.css file for a screen in the project (warning)
// CCB-P1016 unresolved @string/ or @color/ reference, or a skipped
//   malformed/non-hex resource entry (warning)
// CCB-P1017 tap target too small: button/textfield under 8 columns
//   wide or 2 rows tall in wireframe cells; the message also reports
//   the size in dp (F2), but the thresholds stay in cells (warning)
// CCB-P1018 text contrast below threshold: 4.5:1, or 3.0:1 when the
//   leaf rule's font-size is >= 24px (warning)
// CCB-P1019 handler found, button not bound: a click handler whose
//   receiver has no [preview.bindings] entry (warning, always)
// CCB-P1020 declared navigation disagrees with a derived edge for
//   the same button; the declared edge stands (warning, always)
// CCB-P1021 XML onClick button not found in any screen: an
//   android:onClick handler whose bound component label appears in
//   no screen's component tree, so its edges have nowhere to attach
//   (warning, always)
// CCB-P1022 scenario entry does not match the project: a scenario
//   names a screen, branch entry, list, field, or button that does
//   not exist (or a list that already has static rows, or a button
//   with no unknown argument to fill), so that entry is skipped
//   (warning, always — a scenario never fails the build)
// CCB-P1023 trace step does not match the project: a trace names
//   an unknown screen, an edge no screen carries, or a branch entry
//   that does not exist in the named screen, so that step (or its
//   unmatched part) is skipped (warning, always — a trace never
//   fails the build)
// CCB-P1024 ambiguous function reference: a click-handler method
//   reference or a helper call names a top-level fun defined in
//   more than one flow file, so no edge is recorded for it (warning,
//   always — the reference is never guessed)

// Lengths: px values scale to columns/rows by interior_cols /
// phone_dp_width; % values are relative to the parent content size.
typedef enum { PV_LEN_AUTO, PV_LEN_PX, PV_LEN_PCT } pv_len_kind;

typedef struct {
  pv_len_kind kind;
  long value; // px value, or percent 0-100
} pv_len;

// One merged CSS rule: later declarations for the same selector override
// earlier ones property by property. Unset properties keep has_* = 0.
typedef struct {
  char *selector; // "screen" or a lowercase component type, e.g. "button"
  // Scope: NULL means the rule applies to every screen (common.css);
  // otherwise the name of the one screen it applies to (<Screen>.css).
  // Added additively: existing rules with scope NULL keep matching.
  char *scope;
  char *file;
  long line; // line of the first declaration for this selector
  int has_display, display_flex; // display: block (default) or flex
  int has_flex_dir, flex_row; // flex-direction: column (default) or row
  int has_width, has_height;
  pv_len width, height;
  int has_padding, has_margin;
  long padding[4]; // top, right, bottom, left (px)
  long margin[4]; // top, right, bottom, left (px)
  char *bg, *fg; // color as written (#rrggbb or a named color), NULL unset
  int has_font_size;
  long font_size; // px; sets the text-leaf line height (F1) and the
                  // large-text buckets (P1018 threshold, F1 width)
  int has_position, absolute; // position: static (default) or absolute
  int has_top, has_left;
  long top, left; // px
  int has_gap;
  long gap; // px
  int has_text_align, text_align; // 0 left (default), 1 center, 2 right
  int has_border_width;
  long border_width; // px; a scaled value >= 1 cell draws a 1-cell outline
  char *border_color; // hex or named, NULL unset (default: fg, else none)
  int has_flex_wrap, flex_wrap; // 0 nowrap (default), 1 wrap; rows only
} pv_rule;

typedef struct pv_box pv_box;

// A component box. Children are individually arena-allocated; the
// children array itself may move as it grows, so never hold a pointer
// to an element of it across an append.
struct pv_box {
  char *type; // lowercase: "screen", "column", "row", "text", "button",
              // "list", "textfield", "image", "box", "spacer", "branch",
              // "branch-entry", or a generic type for unknown constructs
  char *label; // first string argument (text content), NULL when absent
  pv_box **children;
  size_t nchildren, capchildren;
  // Layout result, in frame columns/rows relative to the screen
  // interior origin (0,0 is the first content cell inside the frame).
  long x, y, w, h;
  // Scaled padding, set by layout from the rule's px padding with the
  // same interior/phone_dp_width scale used for sizes. The renderer
  // paints leaf content inset by these values.
  long pad_left_cols, pad_right_cols, pad_top_rows, pad_bottom_rows;
  // Border outline, set by layout from the rule's border-width: 1
  // when the scaled width is >= 1 cell, else 0. The renderer draws
  // the outline around the box; layout insets content by this.
  int has_border;
  const pv_rule *rule; // resolved during layout; NULL when no rule matches
  int is_branch; // true for when/if wrapper nodes
  // Scenario (Chunk 1 runtime input, arena-owned): values supplied by
  // a scenario, never by static analysis. scenario_text is a
  // textfield's scenario content, shown as "[label: text]" — the
  // static label is never replaced. from_scenario marks boxes the
  // scenario merge created (list rows for a childless list).
  // scenario_picked marks a branch entry a scenario picked (the
  // other entries still render; a pick marks, it never hides).
  char *scenario_text;
  int from_scenario;
  int scenario_picked;
  // Chunk 2: the trace equivalent of scenario_picked — a branch
  // entry the applied trace picked (the step's branch). Trace and
  // scenario picks are independent flags: both overlays may apply,
  // and each value keeps its own provenance.
  int trace_picked;
};

typedef struct {
  char *from; // enclosing component label (or type when unlabeled)
  char *target; // NULL when the destination is dynamic
  int unknown; // true when the destination could not be resolved statically
  // Arguments after the target, arena-owned. Each entry is the
  // argument's literal text, the raw "name = value" text for a named
  // static argument, or NULL (= unknown: identifier, template,
  // expression, or call). Empty when the nav call has no extra args.
  char **args;
  size_t nargs;
  // Parallel to args: 1 when the entry is a named "name = value"
  // argument (shown as written), 0 for a literal (shown quoted).
  // Internal only: JSON carries args alone, never this array.
  int *args_named;
  // Chunk 1: parallel to args: 1 when that entry was filled by a
  // scenario (the static argument was unknown); NULL when no
  // scenario filled any argument of this edge. A filled argument is
  // a literal in args, so without this array its provenance would be
  // lost. Drives the " [scenario]" mark in the tree and
  // "args_provenance" in JSON. Arena-owned.
  int *args_scenario;
  // Chunk 2: parallel to args_scenario, for arguments filled by a
  // trace step (positional: step arg k fills edge arg k when that
  // slot was unknown). A slot is filled by at most one source: the
  // trace only fills slots still NULL after any scenario, so the
  // two arrays never mark the same slot.
  int *args_trace;
  // Chunk 2: set when an applied trace records this edge as taken
  // (a step named this screen and this button). Drives the
  // " [trace]" mark on the tree edge line and "taken":true in JSON.
  int trace_taken;
  // Slice 1: 1 when this edge was derived from a click handler
  // (listener lambda, onClick assignment, or named handler) rather
  // than declared by a nav() call in the flow. Internal only: the
  // JSON shape and the human tree render derived edges exactly like
  // declared ones. The post-load reconciliation in pv_project_load
  // drops derived edges that a declared edge for the same button
  // duplicates or disagrees with.
  int derived;
  // Slices 2/3: the branch this edge is conditional on, e.g.
  // "if ready: then" or "when state: is Loaded", or NULL when the
  // nav call is unconditional. A label only, never evaluated. Shown
  // in the tree and walk edge lists as " [branch]", and in JSON as
  // the edge's "branch" field (null when unconditional). Edges
  // deduplicate on branch too: the same destination under two
  // branches is two edges.
  char *branch;
} pv_edge;

// A derived edge from an XML android:onClick handler (Slice 3),
// collected during flow parsing and attached to screens by
// pv_project_load after every flow file is parsed (the button's
// screen is the one whose component tree contains the bound label).
// Field-for-field the edge the screen will carry, plus the onClick
// method name it came from (used for the CCB-P1021 diagnostic when
// no screen contains the label). All strings are arena-owned.
typedef struct {
  char *method;
  char *from;
  char *target;
  int unknown;
  char **args;
  size_t nargs;
  int *args_named;
  char *branch;
} pv_pending_edge;

typedef struct {
  char *name;
  pv_box *root; // type "screen"
  pv_edge *edges;
  size_t nedges, capedges;
  int reachable; // computed by pv_project_diagnose
  // Chunk A: 1 when this screen came from a nav-graph destination
  // with no flow sketch (a node with unknown layout, not an error).
  // Graph-only screens skip the CCB-P1007 empty-screen warning:
  // emptiness is definitional for them, not a finding.
  int graph_only;
} pv_screen;

typedef struct {
  ccb_arena *arena;
  pv_screen **screens;
  size_t nscreens, capscreens;
  pv_rule *rules; // merged, one per selector, in first-seen order
  size_t nrules, caprules;
  ccb_diag_list diags;
  long frame_width; // terminal columns including frame borders
  long interior_cols; // frame_width - 2
  long phone_dp_width; // from config, for px scaling
  int ascii; // --ascii: ASCII frame characters instead of Unicode
  int strict; // --strict / [preview] strict: subset warnings become errors
  // Transient: name of the screen currently being laid out, used by
  // layout to skip rules scoped to another screen. NULL matches all
  // rules; set by pv_layout_screen, cleared when it returns.
  const char *layout_screen_name;
  // Chunk 1: name of the scenario applied at load (arena-owned), or
  // NULL when no scenario was applied. Emitted as "scenario" in
  // JSON only when set, so the no-scenario output is byte-identical
  // to before.
  char *scenario_name;
  // Chunk 2: set when a trace was applied (after load, by the CLI
  // or by a test calling pv_trace_apply). trace_step_count is the
  // number of steps in the trace file, whether or not every step
  // matched. Emitted as "trace":{"steps":N} in JSON only when set.
  int trace_applied;
  long trace_step_count;
  // Chunk A: [project] package (arena-owned, or NULL when the
  // config has none), used to normalize Android class names before
  // comparison.
  char *package;
  // Chunk A: manifest activity names, normalized (arena-owned).
  // A nav target matching one of these is known, not a P1005 —
  // but manifest activities are NOT screens: unlike graph
  // destinations, they do not become nodes.
  char **manifest_activities;
  size_t nmanifest_activities;
} pv_project;

// ---------- CSS subset parser + layout (ccb_preview_css.c) ----------

// Parse CSS text into merged rules appended to *rules_out (arena).
// Unknown properties warn (CCB-P1001) or error under strict; parse
// errors warn (CCB-P1002) and keep what parsed. Always returns CCB_OK;
// failures surface as diagnostics, never as crashes.
ccb_status pv_css_parse(ccb_arena *arena, const char *text, const char *file,
  int strict, ccb_diag_list *diags, pv_rule **rules_io, size_t *nrules_io);
// Merge one fully-parsed rule (already merged within its own file)
// into the merged rule array, replicating pv_css_parse's cross-file
// semantics exactly: a rule for a selector this file already owns
// (same file, or same scope) merges in place, property by property;
// a per-screen file (scope != NULL) redeclaring a selector first
// seen elsewhere appends a copy of the first matching rule carrying
// the new file and scope, leaving the earlier rule untouched; a
// common file (scope == NULL) merges into the first matching rule;
// otherwise the rule is appended with the given file and scope.
// Only has_* properties copy on a merge — selector, file, line, and
// scope keep the target's values (a split copy takes the base rule's
// line, like the parser's own split path). Lets the LSP server merge
// per-file cached rules in load order instead of re-parsing every
// CSS file on each rebuild; pv_project_load_scenario uses the same
// function, so both paths share one merge definition.
void pv_css_rule_merge(ccb_arena *arena, pv_rule **rules_io,
  size_t *nrules_io, const pv_rule *src, const char *file,
  const char *scope);
// Compute x/y/w/h for a screen tree. Rules are matched by box type
// ("screen" for the root). Absolute boxes do not affect parent height.
// Emits CCB-P1003/P1004/P1009 diagnostics (overlap/off-screen/clip).
void pv_layout_screen(pv_project *p, pv_screen *screen);
// F2 DP mapping for a laid-out size. w_dp = round(cols *
// phone_dp_width / interior_cols) — 1 column is ~10.26dp at the
// default 40-column phone frame (38 interior columns, 390dp); h_dp =
// rows * 10 (one row is ~10dp). Estimates from the cell grid, not
// measurements; 0 for a non-positive input or scale.
long pv_cols_to_dp(long cols, long interior_cols, long phone_dp_width);
long pv_rows_to_dp(long rows);

// ---------- Kotlin structure parser (ccb_preview_flow.c) ----------

// Parse flow text into screens appended to *screens_io (arena).
// Top-level `fun Name(` bodies are screens; component calls nest by
// braces; nav("X") / navigateTo("X") calls become edges; when/if
// become branch nodes labeled with their conditions, never evaluated.
// Unknown constructs warn (CCB-P1012) and become generic boxes.
// Always returns CCB_OK; failures surface as diagnostics.
ccb_status pv_flow_parse(ccb_arena *arena, const char *text, const char *file,
  ccb_diag_list *diags, pv_screen ***screens_io, size_t *nscreens_io);
// Slice 1 variant of pv_flow_parse: bind_keys/bind_vals are parallel
// arrays (borrowed from the caller, e.g. the config's
// [preview.bindings] table) mapping code identifiers to component
// labels. A NULL/empty binding set means every handler receiver is
// unbound (CCB-P1019). pv_flow_parse is this function with no
// bindings. See docs/preview.md for the recognized handler shapes.
ccb_status pv_flow_parse_bindings(ccb_arena *arena, const char *text,
  const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
  size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind);
// Slices 2/3 full variant: additionally, onclick_methods/labels are
// parallel arrays (borrowed) mapping android:onClick method names to
// already-bound component labels — a top-level fun named by an
// onClick method is a handler, not a screen, and its nav calls are
// appended to *pending_io (arena-grown, count in *npending_io) for
// the caller to attach to screens. graph_keys/vals map navigation
// destination ids (e.g. "detail") to screen names for navigate()
// calls inside handler bodies. NULL/empty for any set means that
// input is absent. pv_flow_parse_bindings is this function with no
// onClick mapping, no graph, and no pending output.
ccb_status pv_flow_parse_full(ccb_arena *arena, const char *text,
  const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
  size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind,
  char **onclick_methods, char **onclick_labels, size_t nonclick,
  char **graph_keys, char **graph_vals, size_t ngraph,
  pv_pending_edge ***pending_io, size_t *npending_io);
// Chunk B: whole-project function index. An index records every
// top-level fun name and the flow file holding it, over borrowed
// (label, text) pairs — the texts must outlive every parse that
// uses the index. pv_flow_parse_indexed is pv_flow_parse_full with
// an index: a click-handler method reference or a helper call that
// names a fun in another file resolves through the index when the
// name is unique across files; an ambiguous name (same fun in two
// files) records nothing and warns CCB-P1024. A NULL index behaves
// exactly like pv_flow_parse_full. The index owns only its fun
// table; labels and texts stay borrowed and are never freed here.
typedef struct pv_fun_index pv_fun_index;
pv_fun_index *pv_fun_index_build(ccb_arena *arena, char **labels,
  char **texts, size_t nfiles);
void pv_fun_index_free(pv_fun_index *index);
// Chunk C: warn CCB-P1025 for every nav call in the index files
// that no parse attributed to an edge (nested block, skipped
// statement, or helper never scanned from a button). Runs after all
// files are parsed, before the index is freed; a NULL index or diag
// list is a no-op.
void pv_sweep_unattributed_navs(pv_fun_index *index,
  ccb_diag_list *diags);
ccb_status pv_flow_parse_indexed(ccb_arena *arena, const char *text,
  const char *file, ccb_diag_list *diags, pv_screen ***screens_io,
  size_t *nscreens_io, char **bind_keys, char **bind_vals, size_t nbind,
  char **onclick_methods, char **onclick_labels, size_t nonclick,
  char **graph_keys, char **graph_vals, size_t ngraph,
  pv_pending_edge ***pending_io, size_t *npending_io,
  pv_fun_index *index);

// Render one edge's argument list into out, in tree/walk format:
// "(...)" with literals quoted, named args as written, unknown args
// as "unknown"; nothing at all when the edge has no arguments.
// Shared by the logic tree (ccb_preview_flow.c) and walk mode so
// both show the same argument text.
void pv_render_edge_args(ccb_str *out, const pv_edge *e);

// ---------- Rendering (ccb_preview_render.c) ----------

// Resolve a CSS color to RGB (hex #rgb/#rrggbb, or the renderer's
// named-color table, case-insensitive, surrounding spaces ignored).
// Returns 1 and fills *r/*g/*b on success, 0 when the color is
// missing or unresolvable (including an unresolved @color/ literal).
// Shared by the renderer and the P1018 contrast check so both use
// one RGB resolution.
int pv_color_rgb(const char *s, long *r, long *g, long *b);
// F1 scaled text width in cells, for text-align: the label's display
// width times the rule's font-size bucket factor (small, font-size
// < 14px: 4/5; base, 14-23px: 1; large, >= 24px: 5/4; no font-size
// rule: 1), rounded up. The buckets are statistical constants, not
// glyph measurement — at the base size the renderer already paints
// one cell per character, so the base factor is 1 by construction.
// Painting still uses raw cells, so only alignment shifts; the
// P1009 clip check keeps comparing the actually-painted width.
long pv_scaled_text_width(long display_width, const pv_rule *rule);

// Wireframe for one laid-out screen, including the phone frame. When
// color is enabled, background/foreground colors approximate the CSS
// colors with ANSI 256-color codes; color is disabled under NO_COLOR,
// a dumb terminal, or --ascii. Text is clipped to its box, never wrapped.
void pv_render_screen(ccb_str *out, pv_screen *screen, long frame_width,
  int ascii, int color);
// Logic tree for the whole project: one block per screen, edges with
// their source labels, branch annotations, reachability marks.
// Format (exact, golden-tested). Edges with arguments show them in
// parens after the target: literals quoted, unknown args as
// "unknown", named args as written (name = value):
//   Home (entry)
//     -> Detail via "Open note"
//     -> Detail("note-42", unknown) via "Open note"
//     -> Detail via "Open note" [if ready: then]
//     -> unknown via "More"
//   Detail
//     branches: when state [is Loading, is Loaded, else]
//     -> Home via "Back"
void pv_render_logic_tree(ccb_str *out, pv_project *p);

// ---------- Orchestration (ccb_preview.c) ----------

// Load a project: read every *.css under cfg->preview_styles and every
// *.kt under cfg->preview_flow (sorted, deterministic), parse, and lay
// out. Flow parsing uses cfg's [preview.bindings] table (Slice 1
// handler attribution), and after all flow files are parsed the
// declared-beats-derived reconciliation drops derived edges that a
// declared edge for the same button duplicates or disagrees with
// (CCB-P1020 on disagreement). frame_width of 0 means the default (40); smaller widths clamp
// to 24 with a CCB-P1013 warning. Flow files are parsed first, then
// CSS: common.css first (scope NULL, all screens), then per-screen
// files in sorted order (scope = the screen name); any other CSS file
// is skipped with a CCB-P1015 warning. Returns CCB_ERR_CONFIG when
// the config has no usable [preview] section; that case is the
// caller's "preview not applicable" path, not a preview failure.
// When diags is non-NULL, load parses into it and, on success,
// ownership of its contents transfers to the project (the caller's
// list is left empty and must not be freed again); on
// CCB_ERR_CONFIG the caller keeps ownership. When diags is NULL the
// project uses its own list. No SDK/toolchain is ever consulted.
ccb_status pv_project_load(const ccb_config *cfg, long frame_width, int ascii,
  int strict, ccb_diag_list *diags, pv_project **out);
// Full variant: applies scenario (borrowed from the config, NULL for
// none) after parsing and @string/@color resolution but before
// layout, so scenario list rows size the list and scenario values
// are laid out like any other content. pv_project_load is this
// function with no scenario. Scenario entries that match nothing
// warn (CCB-P1022) into the diag list; they never fail the load.
ccb_status pv_project_load_scenario(const ccb_config *cfg,
  long frame_width, int ascii, int strict, ccb_diag_list *diags,
  const ccb_scenario *scenario, pv_project **out);
void pv_project_free(pv_project *p);

// One pre-parsed CSS file for assembly: the rules pv_css_parse
// produced for that file alone (merged within the file only), plus
// the parse diagnostics for the file, replayed by assembly in load
// order. Paths and rule strings are borrowed; the diag list is
// borrowed and never consumed (assembly copies each diagnostic).
typedef struct {
  const char *path;
  pv_rule *rules;
  size_t nrules;
  ccb_diag_list *diags;
} pv_css_entry;

// Assemble a project from pre-parsed inputs: attach pending onClick
// edges, reconcile declared-beats-derived edges, merge CSS entries
// in load order (common files first, then per-screen files, with
// CCB-P1015 for a per-screen file naming no screen), resolve
// @string/@color resources, apply the scenario, adopt the diag list,
// and lay out every screen. Parse diagnostics for flow files are the
// caller's job (pushed before this call, in file order); CSS parse
// diagnostics replay from the entries during the CSS loop, so the
// final diag order matches pv_project_load_scenario exactly.
// pv_project_load_scenario is this function over freshly parsed
// inputs; the LSP server calls it over cached per-file parses, so
// both paths share one assembly definition and one output shape.
// The project shell (arena, frame width, strict, phone width) is
// already initialized; this function fills screens, rules, diags,
// and layout. Does not diagnose (pv_project_diagnose) and does not
// apply a trace — both stay caller steps, as in the CLI.
// Chunk A: assembly also appends one node per nav-graph
// destination with no matching flow screen (graph_vals, normalized
// at scan time; matched with pv_names_match, so a destination
// spelling like DetailActivity never duplicates a Detail screen),
// and records the package + manifest activities on the project
// for pv_project_diagnose. Graph nodes are added before
// distribution, CSS, and layout, so they flow through assembly
// like any screen (except they carry no boxes, so distribution,
// CSS scoping by label, scenario row/field application, and layout
// all skip them naturally).
void pv_project_assemble(pv_project *p, pv_screen **screens,
  size_t nscreens, pv_pending_edge **pending, size_t npending,
  pv_css_entry *css_entries, size_t ncss, const char *resources_dir,
  const ccb_scenario *scenario, ccb_diag_list *diags,
  char **graph_vals, size_t ngraph, char **manifest_activities,
  size_t nmanifest_activities, const char *package);

// Scan the nav-graph and layout XML inputs for a config: the graph
// id -> screen name map and the onClick method -> bound label map
// (resolved through [preview.bindings]). The returned arrays are
// arena-grown; the scan's temporary string arrays are freed before
// return. A layout button with no binding warns (CCB-P1019) into
// diags, exactly as in pv_project_load_scenario, which calls this
// before flow parsing. The LSP server reuses it on every rebuild
// (re-scanning only when the XML inputs change) so both paths share
// one scan definition.
void pv_preview_scan_xml(const ccb_config *cfg, ccb_arena *arena,
  char ***graph_keys_io, char ***graph_vals_io, size_t *ngraph_io,
  char ***onclick_methods_io, char ***onclick_labels_io,
  size_t *nonclick_io, ccb_diag_list *diags);

// Location of the layout/nav-graph inputs: [preview] layout /
// nav_graph when set (strict: the config layer requires the path),
// else the first EXISTING directory among <paths res>/layout or
// navigation and <resources>/layout or navigation, else NULL.
// [paths] res is preferred: the build already knows the res root,
// and [preview] resources is documented as the values directory,
// whose /layout never exists — so a values-dir resources must fall
// through to res instead of shadowing it with an empty input set.
// Malloc'd, caller frees. Shared with the LSP server, which
// watches the same inputs.
char *pv_preview_layout_path(const ccb_config *cfg);
char *pv_preview_nav_path(const ccb_config *cfg);
// Normalize an Android class name for comparison: strip a leading
// '.', strip the project package prefix + '.', then take the
// simple name (after the last '.'). So ".DetailActivity",
// "com.example.DetailActivity", and "DetailActivity" all normalize
// to "DetailActivity". Malloc'd; NULL in gives NULL out. A NULL
// package only skips the prefix strip.
char *pv_normalize_class(const char *package, const char *name);
// True when two names denote the same screen: equal after
// normalization, or equal after stripping one trailing "Activity"
// or "Fragment" from exactly one side (DetailActivity matches
// Detail, and Detail matches DetailActivity; DetailActivity does
// NOT match DetailFragment). Deterministic; anything else does
// not match, never a guess.
int pv_names_match(const char *package, const char *a, const char *b);
// Find a screen by target name using pv_names_match, preferring an
// exact normalized match over a suffix match, or NULL when no
// screen matches. The project's package (when set) drives
// normalization; a NULL package degrades to leading-dot + simple
// name only.
pv_screen *pv_screen_find(pv_project *p, const char *target);
// Collect normalized activity names from the manifest at path
// (android:name on <activity> elements), or none when the path is
// NULL or the file is missing/unreadable. Arena-owned strings;
// the arrays themselves are arena-grown. A malformed manifest
// yields whatever tags scan cleanly, never an error: preview must
// not fail on a manifest it cannot fully read.
void pv_manifest_activities(ccb_arena *arena, const char *manifest_path,
  const char *package, char ***acts_io, size_t *nacts_io);
// Error classification for one diagnostic code under a strict flag:
// CCB-P1005 and CCB-P1008 are always errors; CCB-P1001 and
// CCB-P1014 count as errors only when strict is set; everything
// else is a warning — including CCB-P1019, CCB-P1020, CCB-P1021, and
// CCB-P1022, which are warnings always and never fail the build
// gate. This is the single definition of the rule that
// pv_project_diagnose counts with and that the preview gate
// (ccb_preview_gate) collects with; keep the three in sync.
// CCB-P1023, CCB-P1024, and CCB-P1025 fall through to the default
// (warning always), like CCB-P1022, so they need no case here.
int pv_diag_is_error(const char *code, int strict);
// Run the diagnostics pass: overlap/off-screen/clip are emitted during
// layout; this pass adds missing-target (error), duplicate-screen
// (error), unreachable (warning), empty-screen (warning), and
// no-screens (warning) diagnostics, and marks each screen reachable.
// Returns the number of errors. The parsers emit CCB-P1001/CCB-P1014
// with the same codes in both modes; under strict this pass counts
// those codes as errors (promotion by counting, here).
long pv_project_diagnose(pv_project *p);
// JSON form of the same data the human render shows, one object, no
// trailing newline, deterministic (stable order, no timestamps):
// {"event":"preview","screens":[{"name":...,"entry":bool,
// "reachable":bool,"boxes":{type,label,x,y,w,h,w_dp,h_dp,children:[...]
// (the root box is the tree root)},"edges":[{"from":...,"target":...|null,
// "unknown":bool}],"branches":[{"label":...,"entries":[...]}]}],
// "diagnostics":[{"code":...,"phase":...,"message":...,"file":...|null,
// "line":...}]}. Branches are collected depth-first from the tree.
// The object carries "version":2; each edge carries "args" (an array
// of string-or-null, [] when the nav call has no extra arguments)
// and "branch" (the Slice 2 branch label string, or null when the
// edge is unconditional). w_dp/h_dp are additive per-box fields, so
// the version stays 2. Scenario fields are additive the same way
// and appear only when a scenario supplied them, so no-scenario
// JSON is byte-identical to before: "scenario" (top level, the
// scenario name) when one was applied; per-edge "args_provenance"
// (parallel to "args", null for static entries, "scenario" for
// filled ones) when an edge has a filled argument; per-branch
// "picked" ({"entry":..., "provenance":"scenario"}) when a branch
// was picked; per-box "provenance":"scenario" on scenario-created
// boxes (list rows) and "value"/"value_provenance":"scenario" on a
// textfield with scenario content. Trace fields are additive the
// same way and appear only when a trace was applied: "trace"
// (top level, {"steps":N} for steps in the file) when one was
// applied; per-edge "taken":true when the trace took the edge;
// "trace" entries in "args_provenance" for trace-filled arguments;
// per-branch "picked" with "provenance":"trace" (a trace pick wins
// over a scenario pick of a different entry).
void pv_project_json(ccb_str *out, pv_project *p);
// JSON for a subset of screens, in the given order: the same object
// as pv_project_json, but the "screens" array contains only these
// screens (repeats appear once per occurrence). Used by --storyboard;
// pv_project_json is this function over every screen, in order.
void pv_project_json_screens(ccb_str *out, pv_project *p,
  pv_screen **screens, size_t nscreens);
// JSON for one screen object, exactly as it appears in the "screens"
// array of pv_project_json_screens: name, entry, reachable, boxes,
// edges, branches. Used by the LSP server's ccb/screenInfo, so the
// screen object never drifts from the project JSON shape.
void pv_project_json_screen(ccb_str *out, pv_project *p,
  pv_screen *screen);

#endif
