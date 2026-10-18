/*
 * ratpipe_abi.h — the RatMode third-party pipe ABI (Arch38).
 *
 * SOURCE OF TRUTH. The SDK repo (ratmode-pipe-sdk) carries a byte-identical
 * copy and its CI fails if the two differ (OD7). Pure C99, no JUCE, no C++:
 * this header must compile standalone as C and as C++. Nothing but POD
 * structs, plain enums, C strings and function pointers crosses the boundary.
 *
 * Versioning
 * ----------
 *   RATPIPE_ABI_MAJOR  bumps only for breaking changes. The host accepts a
 *                      descriptor whose abi_major == RATPIPE_ABI_MAJOR.
 *   RATPIPE_ABI_MINOR  bumps for additive changes: new fields are appended at
 *                      the END of a struct and every struct begins with
 *                      `uint32_t struct_size`, filled by whoever creates the
 *                      struct with sizeof(that struct) as THEY compiled it.
 *                      Two kinds of struct, two rules:
 *
 *                      SINGLETON structs (RatPipeDescriptor, RatPipeHostInfo,
 *                      RatPipeTransport) are passed by pointer, one at a
 *                      time. A reader touches a field only when
 *                      offsetof(field) + sizeof(field) <= struct_size, and
 *                      the host refuses a descriptor whose struct_size is
 *                      SMALLER than the size the host was built with. New
 *                      fields are appended at the end.
 *
 *                      ARRAY-ELEMENT structs (RatPipeParamDesc, RatPipeEvent)
 *                      are walked with each side's own sizeof as the stride,
 *                      so a size mismatch mis-reads every element after the
 *                      first. For these, struct_size must EQUAL sizeof
 *                      EXACTLY on both sides: the host rejects a descriptor
 *                      whose params[] elements differ from its own sizeof,
 *                      and the extension must ignore an events[] array whose
 *                      elements differ from its own sizeof. New fields are
 *                      carved out of the trailing `reserved[]` padding, which
 *                      keeps sizeof constant within an ABI major; when the
 *                      padding runs out, the major bumps.
 *
 * Entry point
 * -----------
 *   The bundle exports exactly one symbol (everything else hidden):
 *
 *       const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host);
 *
 *   It returns a pointer to a STATIC descriptor that stays valid for the
 *   process lifetime (the host never dlcloses a bundle). It may return NULL to
 *   refuse the host (ABI / app version / sample rate it cannot serve). It must
 *   be cheap and side-effect free: no large allocation, no files, no threads
 *   (contract M15) — RatMode never calls it at scan time, only at load.
 *
 * Threading contract (contract M11) — read this twice
 * ---------------------------------------------------
 *   The host calls these on its MESSAGE thread:
 *       create, destroy, prepare, set_param, get_param
 *   and this on its AUDIO thread, concurrently with the above:
 *       render
 *
 *   Guarantees the host gives the extension:
 *     - `destroy` is called only after the host has quiesced render: no render
 *       call is in flight and none will follow for that instance.
 *     - RatMode additionally closes its render gate around `prepare`, so a
 *       prepare never overlaps a render for the same instance. Other hosts
 *       (the SDK's offline validator) may not — size everything for
 *       `max_block` in create and treat prepare as a sample-rate change.
 *     - `render` for one instance is never called re-entrantly or from two
 *       threads at once.
 *
 *   Obligations on the extension:
 *     - `render` MUST NOT allocate, free, lock, block, do file or network IO,
 *       or call into any runtime that might (contract M1). All buffers are
 *       sized in create/prepare for `max_block` samples.
 *     - The ONLY state shared between set_param (message thread) and render
 *       (audio thread) is per-parameter atomics written by set_param and
 *       read by render (std::atomic<float> or relaxed std::atomic<int>;
 *       in C: _Atomic float / atomic_store_explicit). No other shared
 *       mutable state. get_param reads the same atomics (contract M2).
 *     - The extension must not spawn threads that touch audio state.
 *     - No C++ exception may cross this boundary (undefined behaviour). The
 *       SDK base class catches everything inside the bundle; the host wraps
 *       every call in catch(...) as belt and braces, but a SIGSEGV is fatal.
 *
 * Audio contract
 * --------------
 *   `render` is ADDITIVE into L and R (contract M10): never clear them, never
 *   write outside [0, n). Output must be finite and bounded: no NaN/Inf, peak
 *   <= 2.0 linear under any parameter combination (contract M3). RatMode
 *   sanitises anyway (non-finite block -> silence + strike; hard clamp +/-4;
 *   three strikes -> the pipe is faulted and silenced) — the sanitiser is a
 *   safety net, not a feature.
 *
 *   Events arrive once per block, in dispatch order (sample_offset ascending
 *   within a block, offsets already clamped to [0, n-1]). Every element has
 *   struct_size == sizeof(RatPipeEvent) as the host compiled it; an extension
 *   must treat any other value as "not my ABI" and skip the array. Pending control
 *   events (ALL_OFF, RELEASE_HELD, FLUSH) are delivered at sample_offset 0 of
 *   the next block, so the extension never needs cross-thread handling beyond
 *   its parameter atomics.
 *
 * Parameter contract
 * ------------------
 *   Values cross the ABI normalised 0..1 (or 0..1.5 for GAIN_STYLE); the
 *   extension maps to engineering units internally (contract M9). Param ids
 *   are `<macro_prefix>.<path>`: lowercase letters, digits, `_` and `.`;
 *   the prefix is 2..6 lowercase letters and must not be one of the reserved
 *   prefixes the host already routes: fx sculpt synth gain sample dry ra rb
 *   an sv sn sub0 sub1 sub2 sub3 au_param __  (contract M8). Ids are stable
 *   across versions: renaming one orphans saved songs — add new ids, keep
 *   old ones accepted.
 *
 *   ALL sound-affecting state must be a declared parameter (contract M7).
 *   After an ALL_OFF + FLUSH and set_param of every parameter, two instances
 *   must be indistinguishable: RatMode re-derives every parameter on every
 *   song switch (descriptor default, then the song's stored values) and
 *   relies on this to stop one song's state bleeding into another.
 */

#ifndef RATPIPE_ABI_H_INCLUDED
#define RATPIPE_ABI_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */

#define RATPIPE_ABI_MAJOR 1u
#define RATPIPE_ABI_MINOR 0u

/* The one exported symbol, as a string for dlsym. */
#define RATPIPE_ENTRY_SYMBOL "ratpipe_entry"

/* Exported-symbol decoration for the bundle side. The host never uses this. */
#if defined(_WIN32)
  #define RATPIPE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
  #define RATPIPE_EXPORT __attribute__((visibility("default")))
#else
  #define RATPIPE_EXPORT
#endif

/* ── Part kind ───────────────────────────────────────────────────────────── */

/* Decides the sequencer lane the pipe gets and which events it receives. */
enum RatPipePartKind
{
    RATPIPE_DRUM = 0,   /* drum lane: RATPIPE_EV_TRIGGER events (host PartType::Perc)   */
    RATPIPE_MONO = 1,   /* melodic mono: NOTE_ON/NOTE_OFF, one voice expected (Bass)    */
    RATPIPE_POLY = 2    /* melodic poly: NOTE_ON/NOTE_OFF; host expands chords in M2    */
                        /* (Chords). In M1 a POLY pipe receives the root note only.      */
};

/* ── Parameters ──────────────────────────────────────────────────────────── */

/* RatPipeParamDesc.flags — bit set. */
#define RATPIPE_PARAM_CONTINUOUS   0x0u  /* default: smooth 0..1                      */
#define RATPIPE_PARAM_DISCRETE     0x1u  /* `steps` evenly spaced positions in 0..1  */
#define RATPIPE_PARAM_BIPOLAR      0x2u  /* UI draws the arc from the centre (0.5)   */
#define RATPIPE_PARAM_GAIN_STYLE   0x4u  /* range is 0..1.5 (1.0 = unity), not 0..1  */
#define RATPIPE_PARAM_NOT_MUTABLE  0x8u  /* Mutate never touches this parameter       */

/* RatPipeParamDesc.readout_kind — how the Edit panel prints the value. The
 * extension supplies the mapping itself via readout_min/readout_max (linear)
 * — the host only formats. RATPIPE_READOUT_NONE prints nothing. */
enum RatPipeReadoutKind
{
    RATPIPE_READOUT_NONE = 0,
    RATPIPE_READOUT_HZ   = 1,
    RATPIPE_READOUT_MS   = 2,
    RATPIPE_READOUT_PCT  = 3,
    RATPIPE_READOUT_ST   = 4    /* semitones */
};

/* ARRAY-ELEMENT struct: struct_size must EQUAL sizeof(RatPipeParamDesc) exactly
 * (see Versioning). Future minor versions take fields from `reserved`. */
typedef struct RatPipeParamDesc
{
    uint32_t    struct_size;    /* = sizeof(RatPipeParamDesc), exactly                 */
    const char* id;             /* "<prefix>.<path>", static string, unique per pipe   */
    const char* label;          /* knob label, <= 8 characters                         */
    const char* group;          /* Edit-panel row / section label (may be "")          */
    float       default_value;  /* in [min_value, max_value]                           */
    float       min_value;      /* 0.0 unless BIPOLAR/GAIN_STYLE conventions say else   */
    float       max_value;      /* 1.0, or 1.5 for GAIN_STYLE                          */
    float       mutate_lo;      /* safe band for Mutate; min..max if equal to each other */
    float       mutate_hi;
    uint32_t    flags;          /* RATPIPE_PARAM_*                                      */
    int32_t     steps;          /* DISCRETE only: number of positions (>= 2), else 0    */
    int32_t     readout_kind;   /* RatPipeReadoutKind                                   */
    float       readout_min;    /* engineering value printed at min_value               */
    float       readout_max;    /* engineering value printed at max_value               */
    const char* const* choices; /* DISCRETE only: `steps` static labels, or NULL        */
    uint32_t    reserved[8];    /* zero in ABI 1.0; future minor fields live here       */
} RatPipeParamDesc;

/* ── Host info (host -> extension) ───────────────────────────────────────── */

typedef struct RatPipeHostInfo
{
    uint32_t    struct_size;        /* = sizeof(RatPipeHostInfo) as the host compiled it */
    uint32_t    abi_major;          /* RATPIPE_ABI_MAJOR the host was built with         */
    uint32_t    abi_minor;
    const char* host_name;          /* "RatMode", "ratpipe-host", ...                    */
    const char* host_version;       /* semver string, e.g. "0.1.8"                       */
    double      sample_rate;        /* current device rate at load time (0 if unknown)   */
    int32_t     max_block;          /* largest n render will ever see (0 if unknown)     */
    int32_t     _pad0;
    uint64_t    seed;               /* host seed for DETERMINISTIC_WITH_SEED pipes       */
} RatPipeHostInfo;

/* ── Events (host -> extension, per render call) ─────────────────────────── */

enum RatPipeEventType
{
    RATPIPE_EV_TRIGGER      = 0,  /* drum hit: velocity, sub_index, semitone_offset, step_index */
    RATPIPE_EV_NOTE_ON      = 1,  /* note, velocity, step_index, flags (SLIDE_FROM_PREV, ACCENT) */
    RATPIPE_EV_NOTE_OFF     = 2,  /* note, flags (SLIDE_SOURCE)                                  */
    RATPIPE_EV_ALL_OFF      = 3,  /* silence every voice now (transport stop, key change)        */
    RATPIPE_EV_RELEASE_HELD = 4,  /* move held voices to their release stage (mute edge)         */
    RATPIPE_EV_FLUSH        = 5   /* hard reset: all voices off + clear every tail/delay (KILL)  */
};

/* RatPipeEvent.flags */
#define RATPIPE_EVF_SLIDE_FROM_PREV 0x1u  /* NOTE_ON: previous step slid into this one — glide, don't retrigger */
#define RATPIPE_EVF_ACCENT          0x2u  /* NOTE_ON: 303-style accent                                           */
#define RATPIPE_EVF_SLIDE_SOURCE    0x4u  /* NOTE_OFF: the delayed off of a slide source                        */

/* ARRAY-ELEMENT struct: struct_size must EQUAL sizeof(RatPipeEvent) exactly
 * (see Versioning). Future minor versions take fields from `reserved`. */
typedef struct RatPipeEvent
{
    uint32_t struct_size;       /* = sizeof(RatPipeEvent), exactly; identical for every element    */
    int32_t  type;              /* RatPipeEventType                                                */
    int32_t  sample_offset;     /* 0 <= sample_offset < n of this render call                      */
    int32_t  note;              /* MIDI note 0..127 (NOTE_ON/OFF); 0 otherwise                    */
    float    velocity;          /* 0..1 (TRIGGER, NOTE_ON)                                         */
    int32_t  sub_index;         /* TRIGGER: 0 for a plain lane (reserved for sub-lanes)            */
    int32_t  semitone_offset;   /* TRIGGER: -24..+24 pitch offset from the step                    */
    int32_t  step_index;        /* pattern step that fired this event, -1 if none                  */
    uint32_t flags;             /* RATPIPE_EVF_*                                                   */
    int32_t  chord;             /* M2: host chord type id for POLY pipes; 0 = none                 */
    uint32_t reserved[4];       /* zero in ABI 1.0; future minor fields live here                   */
} RatPipeEvent;

/* ── Transport (host -> extension, per render call) ──────────────────────── */

typedef struct RatPipeTransport
{
    uint32_t struct_size;       /* = sizeof(RatPipeTransport)                   */
    int32_t  playing;           /* 1 while the sequencer runs                   */
    double   bpm;
    double   sample_rate;       /* current render rate                          */
    int64_t  sample_position;   /* absolute samples since the transport started */
    int64_t  tick;              /* absolute sequencer tick at block start       */
    int32_t  ticks_per_beat;    /* PPQ of `tick`                                */
    int32_t  _pad0;
} RatPipeTransport;

/* ── Descriptor (extension -> host; static, no instance needed) ──────────── */

/* RatPipeDescriptor.flags */
#define RATPIPE_FLAG_WANTS_TRANSPORT         0x1u  /* reads RatPipeTransport (host always fills it anyway) */
#define RATPIPE_FLAG_HAS_TAILS               0x2u  /* output continues after the last event (reverb/delay) */
#define RATPIPE_FLAG_DETERMINISTIC_WITH_SEED 0x4u  /* same host seed -> bit-identical output              */

#define RATPIPE_PERFORM_KNOBS 4
#define RATPIPE_PERFORM_KNOB_NONE (-1)

typedef struct RatPipeDescriptor
{
    uint32_t    struct_size;    /* = sizeof(RatPipeDescriptor) as the bundle compiled it */
    uint32_t    abi_major;      /* must equal the host's RATPIPE_ABI_MAJOR               */
    uint32_t    abi_minor;      /* the RATPIPE_ABI_MINOR the bundle was built against    */
    uint32_t    flags;          /* RATPIPE_FLAG_*                                         */

    /* identity — all static strings, valid for the process lifetime */
    const char* id;             /* reverse-DNS ^[a-z0-9]+(\.[a-z0-9_]+)+$ ; the song stores this */
    const char* display_name;   /* <= 24 characters                                             */
    const char* author;
    const char* version;        /* semver of the pipe                                           */
    const char* sdk_version;    /* semver of the SDK it was built with ("" if none)             */
    int32_t     part_kind;      /* RatPipePartKind                                              */
    int32_t     _pad0;
    const char* macro_prefix;   /* 2..6 lowercase letters; every param id starts "<prefix>."    */

    /* parameters */
    const RatPipeParamDesc* params;       /* static array, param_count entries */
    uint32_t                param_count;  /* 0..256                             */
    uint32_t                _pad1;

    /* pre-assigned Perform knobs: param indices, or RATPIPE_PERFORM_KNOB_NONE */
    int32_t     perform_knobs[RATPIPE_PERFORM_KNOBS];

    /* instance lifecycle — see the threading contract at the top */
    void* (*create)(double sample_rate, int32_t max_block, const RatPipeHostInfo* host);
    void  (*destroy)(void* instance);
    void  (*prepare)(void* instance, double sample_rate, int32_t max_block);
    void  (*render)(void* instance, float* left, float* right, int32_t n,
                    const RatPipeEvent* events, int32_t event_count,
                    const RatPipeTransport* transport);
    void  (*set_param)(void* instance, int32_t param_index, float value);
    float (*get_param)(void* instance, int32_t param_index);

    /* reserved for additive minor versions (theme tokens M2, paint / state blob M3) —
     * must be NULL in ABI 1.0 */
    const void* theme;
    void*       reserved[6];
} RatPipeDescriptor;

/* Signature of the exported entry point. */
typedef const RatPipeDescriptor* (*RatPipeEntryFn)(const RatPipeHostInfo* host);

/* Bundle-side declaration (the host never compiles this line). */
#if !defined(RATPIPE_HOST)
RATPIPE_EXPORT const RatPipeDescriptor* ratpipe_entry(const RatPipeHostInfo* host);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RATPIPE_ABI_H_INCLUDED */
