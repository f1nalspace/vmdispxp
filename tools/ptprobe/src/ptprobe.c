/*
 * ptprobe -- asks the display driver to drive the qemu-3dfx passthrough from kernel mode.
 *
 * Plan C wants a DirectDraw/Direct3D HAL inside vmdispxp, and a HAL lives in the kernel. Before
 * a single line of it is worth writing, two things have to be true: the kernel can reach the
 * passthrough at all (phase 0), and it can share it with the OpenGL programs that already use
 * it (phase 1). This program asks the questions; the driver built with
 * make PASSTHROUGH_PROBE=1 does the work inside DrvEscape. See docs/LOG.md [467].
 *
 * Every step leaves a line in QEMU's log as well, so the guest's view here and the host's view
 * there can be laid next to each other.
 *
 *   PTPROBE phase0                          reach QEMU without a session, nothing else runs
 *   PTPROBE session                         a whole session from the kernel, start to end
 *   PTPROBE state                           read only: who owns the passthrough right now
 *   PTPROBE istexture <name> <chunks> <n>   glIsTexture(name) in chunks of n, count the answers
 */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptprobe.h"

#define REPORT_FILE_NAME "C:\\PTPROBE.TXT"

/* QEMU compares this against its own rev_[] before it says "DLL loaded". The Makefile takes
 * it from the wrapper's stamp.c, which carries the same value. */
#ifndef QEMU3DFX_COMMIT_STAMP
#error QEMU3DFX_COMMIT_STAMP must be defined, see the Makefile
#endif

/* Markers in QEMU's log, one per step: 0x9E00 in the high word so they stand out. */
#define LOG_MARKER_BASE          0x9E000000UL
#define LOG_MARKER_PHASE0_BEGIN  (LOG_MARKER_BASE | 0x0010)
#define LOG_MARKER_PHASE0_END    (LOG_MARKER_BASE | 0x001F)
#define LOG_MARKER_SESSION_BEGIN (LOG_MARKER_BASE | 0x0110)
#define LOG_MARKER_SESSION_END   (LOG_MARKER_BASE | 0x011F)
#define LOG_MARKER_STATE         (LOG_MARKER_BASE | 0x0200)
#define LOG_MARKER_STRESS_BEGIN  (LOG_MARKER_BASE | 0x0310)
#define LOG_MARKER_STRESS_END    (LOG_MARKER_BASE | 0x031F)

/* The drawable marker: two sizes nothing else would ever report. */
#define MARK_DRAWABLE_WIDTH  1234
#define MARK_DRAWABLE_HEIGHT 567

#define GLBOOLEAN_RESULT_MASK 0xFFUL
#define FULL_RESULT_MASK      0xFFFFFFFFUL
#define GL_TRUE_VALUE         1UL
#define GL_FALSE_VALUE        0UL
#define GL_VENDOR_NAME        0x1F00UL
#define GL_RENDERER_NAME      0x1F01UL
#define GL_VERSION_NAME       0x1F02UL

#define GL_NO_ERROR_VALUE          0x0000UL
#define GL_INVALID_OPERATION_VALUE 0x0502UL

#define WINDOW_READY_POLL_MILLISECONDS 10
#define WINDOW_READY_TIMEOUT_MILLISECONDS 10000

/* The stress run asks glGetError after every chunk that went wrong, and for comparison after a
 * few that did not. The pause lets the owner run first. */
#define GL_ERROR_SETTLE_MILLISECONDS 50
#define REPORTED_MISMATCH_CHUNKS     40
#define CONTROL_CHUNK_SPACING        250
#define CONTROL_CHUNK_COUNT          8

/* A user-mode mapping lives below 2 GB on XP without /3GB, a kernel mapping above. */
#define KERNEL_ADDRESS_START 0x80000000UL

static FILE *report_file = NULL;

static void report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);

    if (report_file != NULL) {
        va_start(arguments, format);
        vfprintf(report_file, format, arguments);
        va_end(arguments);
        fflush(report_file);
    }
}

static const char *status_name(unsigned long status)
{
    switch (status) {
    case PTPROBE_STATUS_OK:                return "OK";
    case PTPROBE_STATUS_BAD_REQUEST:       return "BAD_REQUEST";
    case PTPROBE_STATUS_NO_DEVICE:         return "NO_DEVICE";
    case PTPROBE_STATUS_MAP_FAILED:        return "MAP_FAILED";
    case PTPROBE_STATUS_OWNED_ELSEWHERE:   return "OWNED_ELSEWHERE";
    case PTPROBE_STATUS_STAMP_REJECTED:    return "STAMP_REJECTED";
    case PTPROBE_STATUS_UNKNOWN_OPERATION: return "UNKNOWN_OPERATION";
    }
    return "?";
}

/* Returns 1 when the escape reached the driver, whatever the driver made of it. */
static int call_probe(HDC screen, PTPROBE_REQUEST *request, PTPROBE_RESPONSE *response)
{
    int escape_result;

    memset(response, 0, sizeof(*response));
    escape_result = ExtEscape(screen, QEMUDISP_ESCAPE_PASSTHROUGH_PROBE, sizeof(*request), (LPCSTR)request, sizeof(*response), (LPSTR)response);
    if (escape_result <= 0) {
        report("FEHLER: ExtEscape lieferte %d -- der Treiber kennt das Escape nicht\n", escape_result);
        return 0;
    }
    return 1;
}

static int simple_operation(HDC screen, unsigned long operation, unsigned long argument0, unsigned long argument1, PTPROBE_RESPONSE *response)
{
    PTPROBE_REQUEST request;

    memset(&request, 0, sizeof(request));
    request.Operation = operation;
    request.Argument[0] = argument0;
    request.Argument[1] = argument1;
    memcpy(request.CommitStamp, QEMU3DFX_COMMIT_STAMP, PTPROBE_STAMP_BYTES);
    return call_probe(screen, &request, response);
}

static void mark_log(HDC screen, unsigned long marker)
{
    PTPROBE_RESPONSE response;

    simple_operation(screen, PTPROBE_OPERATION_MARK_LOG, marker, 0, &response);
    report("Markierung im QEMU-Protokoll: %08lx\n", marker);
}

static const char *owner_meaning(unsigned long owner_word)
{
    const unsigned long register_offset = owner_word & MESAPT_PAGE_OFFSET_MASK;

    if (owner_word == 0)
        return "frei";
    if (register_offset != MESAPT_REGISTER_FUNCTION)
        return "unbekannter Inhalt";
    if (owner_word < KERNEL_ADDRESS_START)
        return "belegt von einem Benutzerprozess (Wrapper oder ICD)";
    return "belegt aus dem Kernel (dieser Treiber)";
}

static int map_and_report(HDC screen)
{
    PTPROBE_RESPONSE response;

    if (!simple_operation(screen, PTPROBE_OPERATION_MAP, 0, 0, &response))
        return 0;
    report("Abbildung: %s\n", status_name(response.Status));
    report("  Registerseite     0x%08lx  <- physisch 0x%08lx\n", response.Value[0], (unsigned long)MESAPT_MM_BASE);
    report("  FIFO-Kopf         0x%08lx  <- physisch 0x%08lx\n", response.Value[1], (unsigned long)PTPROBE_FIFO_HEAD_PHYSICAL);
    report("  Datenkopf         0x%08lx  <- physisch 0x%08lx\n", response.Value[2], (unsigned long)PTPROBE_DATA_HEAD_PHYSICAL);
    report("  FIFO-Ende         0x%08lx  <- physisch 0x%08lx\n", response.Value[3], (unsigned long)PTPROBE_FIFO_TAIL_PHYSICAL);
    report("  Bildtransfer-Ende 0x%08lx  <- physisch 0x%08lx\n", response.Value[4], (unsigned long)PTPROBE_FRAME_TRANSFER_TAIL_PHYSICAL);
    return response.Status == PTPROBE_STATUS_OK;
}

static int read_state(HDC screen, PTPROBE_RESPONSE *response)
{
    if (!simple_operation(screen, PTPROBE_OPERATION_READ_STATE, 0, 0, response))
        return 0;
    report("Zustand: %s\n", status_name(response->Status));
    report("  Bibliotheksregister 0x%08lx\n", response->Value[0]);
    report("  Fenster bereit      %lu\n", response->Value[1]);
    report("  FIFO-Fuellstand     0x%08lx\n", response->Value[2]);
    report("  Besitzerwort        0x%08lx  -> %s\n", response->Value[3], owner_meaning(response->Value[3]));
    report("  Datenfuellstand     0x%08lx\n", response->Value[4]);
    report("  Referenzzaehler     %lu\n", response->Value[5]);
    report("  Stempel             \"%.8s\"\n", response->Text);
    return response->Status == PTPROBE_STATUS_OK;
}

static int call_repeated(HDC screen, unsigned long function_number, unsigned long argument, unsigned long repeat_count, unsigned long expected, unsigned long mask, PTPROBE_RESPONSE *response)
{
    PTPROBE_REQUEST request;

    memset(&request, 0, sizeof(request));
    request.Operation = PTPROBE_OPERATION_CALL_REPEATED;
    request.Argument[0] = function_number;
    request.Argument[1] = argument;
    request.Argument[2] = repeat_count;
    request.Argument[3] = expected;
    request.Argument[4] = mask;
    return call_probe(screen, &request, response);
}

static void get_string(HDC screen, unsigned long name, const char *label)
{
    PTPROBE_RESPONSE response;
    PTPROBE_REQUEST request;

    memset(&request, 0, sizeof(request));
    request.Operation = PTPROBE_OPERATION_GET_STRING;
    request.Argument[0] = FEnum_glGetString;
    request.Argument[1] = name;
    if (!call_probe(screen, &request, &response))
        return;
    report("  %-12s: \"%s\"  (Rueckgabe 0x%08lx)\n", label, response.Text, response.Value[0]);
}

/* --------------------------------------------------------------- phase 0 -- */

static int run_phase0(HDC screen)
{
    PTPROBE_RESPONSE response;

    report("\n### Phase 0: den Passthrough aus dem Kernel erreichen, ohne Sitzung\n");
    if (!map_and_report(screen))
        return 1;
    mark_log(screen, LOG_MARKER_PHASE0_BEGIN);
    read_state(screen, &response);

    simple_operation(screen, PTPROBE_OPERATION_MARK_DRAWABLE, MARK_DRAWABLE_WIDTH, MARK_DRAWABLE_HEIGHT, &response);
    report("wglSetDrawableSize3DFX %dx%d gesendet: %s\n", MARK_DRAWABLE_WIDTH, MARK_DRAWABLE_HEIGHT, status_name(response.Status));
    simple_operation(screen, PTPROBE_OPERATION_MARK_DRAWABLE, 0, 0, &response);
    report("wglSetDrawableSize3DFX 0x0 gesendet (zurueck auf unbekannt): %s\n", status_name(response.Status));

    /* Without a context QEMU refuses the call and logs it (mesapt_mm.c:2374). */
    call_repeated(screen, FEnum_glIsTexture, 1, 1, GL_TRUE_VALUE, GLBOOLEAN_RESULT_MASK, &response);
    report("glIsTexture(1) ohne Sitzung: Rueckgabe 0x%08lx -- erwartet ist eine Ablehnung im QEMU-Protokoll\n", response.Value[0]);

    read_state(screen, &response);
    mark_log(screen, LOG_MARKER_PHASE0_END);
    return 0;
}

/* --------------------------------------------------------------- phase 1 -- */

static int run_session(HDC screen)
{
    PTPROBE_RESPONSE response;
    PTPROBE_REQUEST request;
    DWORD wait_start;
    DWORD waited = 0;
    int window_ready = 0;

    report("\n### Phase 1.1: eine ganze Sitzung aus dem Kernel\n");
    if (!map_and_report(screen))
        return 1;
    read_state(screen, &response);
    mark_log(screen, LOG_MARKER_SESSION_BEGIN);

    memset(&request, 0, sizeof(request));
    request.Operation = PTPROBE_OPERATION_SESSION_BEGIN;
    memcpy(request.CommitStamp, QEMU3DFX_COMMIT_STAMP, PTPROBE_STAMP_BYTES);
    if (!call_probe(screen, &request, &response))
        return 1;
    report("Sitzungsbeginn: %s, Bibliotheksregister danach 0x%08lx, Besitzerwort vorher 0x%08lx\n", status_name(response.Status), response.Value[0], response.Value[1]);
    if (response.Status != PTPROBE_STATUS_OK)
        return 1;

    wait_start = GetTickCount();
    while (waited < WINDOW_READY_TIMEOUT_MILLISECONDS) {
        PTPROBE_RESPONSE state;

        simple_operation(screen, PTPROBE_OPERATION_READ_STATE, 0, 0, &state);
        if (state.Value[1]) {
            window_ready = 1;
            break;
        }
        Sleep(WINDOW_READY_POLL_MILLISECONDS);
        waited = GetTickCount() - wait_start;
    }
    report("Host-Fenster bereit: %s nach %lu ms\n", window_ready ? "ja" : "NEIN", (unsigned long)waited);
    if (!window_ready) {
        simple_operation(screen, PTPROBE_OPERATION_SESSION_END, 0, 0, &response);
        return 1;
    }

    memset(&request, 0, sizeof(request));
    request.Operation = PTPROBE_OPERATION_SESSION_CONTEXT;
    memcpy(request.CommitStamp, QEMU3DFX_COMMIT_STAMP, PTPROBE_STAMP_BYTES);
    if (!call_probe(screen, &request, &response))
        return 1;
    report("Kontext: %s, Pixelformat %lu, SetPixelFormat-Antwort 0x%08lx (0x%08lx = angenommen)\n", status_name(response.Status), response.Value[0], response.Value[1], (unsigned long)MESAGL_MAGIC);

    report("Aus dem Kernel gelesen:\n");
    get_string(screen, GL_VENDOR_NAME, "GL_VENDOR");
    get_string(screen, GL_RENDERER_NAME, "GL_RENDERER");
    get_string(screen, GL_VERSION_NAME, "GL_VERSION");

    call_repeated(screen, FEnum_glIsTexture, 1, 1, GL_FALSE_VALUE, GLBOOLEAN_RESULT_MASK, &response);
    report("glIsTexture(1) im eigenen, frischen Kontext: 0x%08lx (erwartet FALSE)\n", response.Value[0]);

    simple_operation(screen, PTPROBE_OPERATION_SESSION_END, 0, 0, &response);
    report("Sitzungsende: %s\n", status_name(response.Status));

    read_state(screen, &response);
    mark_log(screen, LOG_MARKER_SESSION_END);
    return 0;
}

static int run_state(HDC screen)
{
    PTPROBE_RESPONSE response;

    report("\n### Phase 1.2: nur lesen -- wem gehoert der Passthrough?\n");
    if (!map_and_report(screen))
        return 1;
    mark_log(screen, LOG_MARKER_STATE);
    read_state(screen, &response);
    return 0;
}

/* glGetError in the owner's context, after giving the owner time to run. Inside an open
 * glBegin/glEnd glGetError itself is invalid and answers 0, so it is only asked once glcube has
 * had the CPU for a while and has, in all likelihood, sent its glEnd. */
static unsigned long settled_gl_error(HDC screen)
{
    PTPROBE_RESPONSE response;

    Sleep(GL_ERROR_SETTLE_MILLISECONDS);
    call_repeated(screen, FEnum_glGetError, 0, 1, GL_NO_ERROR_VALUE, FULL_RESULT_MASK, &response);
    return response.Value[0];
}

static int run_istexture(HDC screen, unsigned long texture_name, unsigned long chunk_count, unsigned long calls_per_chunk)
{
    PTPROBE_RESPONSE response;
    unsigned long chunk_index;
    unsigned long total_matches = 0;
    unsigned long total_mismatches = 0;
    unsigned long first_mismatch_value = 0;
    unsigned long first_mismatch_chunk = 0;
    unsigned long chunks_with_mismatch = 0;
    unsigned long runs_reaching_chunk_end = 0;
    unsigned long errors_invalid_operation = 0;
    unsigned long errors_none = 0;
    unsigned long errors_other = 0;
    unsigned long controls_done = 0;
    unsigned long initial_error;
    unsigned long owner_at_start;
    DWORD start_ticks, elapsed_ticks;

    report("\n### Phase 1.3: glIsTexture(%lu) aus dem Kernel, %lu x %lu Aufrufe\n", texture_name, chunk_count, calls_per_chunk);
    if (!map_and_report(screen))
        return 1;
    read_state(screen, &response);
    owner_at_start = response.Value[3];
    mark_log(screen, LOG_MARKER_STRESS_BEGIN);

    call_repeated(screen, FEnum_glIsTexture, texture_name, 1, GL_TRUE_VALUE, GLBOOLEAN_RESULT_MASK, &response);
    report("Erster Einzelaufruf: Rueckgabe 0x%08lx -- TRUE heisst, er lief in einem Kontext, der die Textur kennt\n", response.Value[0]);

    initial_error = settled_gl_error(screen);
    report("glGetError vor dem Dauerversuch (leert den Zustand): 0x%04lx\n", initial_error);

    start_ticks = GetTickCount();
    for (chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        PTPROBE_RESPONSE state;
        unsigned long chunk_mismatches;

        /* Once the owner is gone, QEMU would answer every further call with a warning line of
         * its own. Stop instead of flooding the log. */
        simple_operation(screen, PTPROBE_OPERATION_READ_STATE, 0, 0, &state);
        if (state.Value[3] != owner_at_start) {
            report("Besitzer gewechselt (0x%08lx -> 0x%08lx) vor Paket %lu -- abgebrochen\n", owner_at_start, state.Value[3], chunk_index);
            break;
        }
        if (!call_repeated(screen, FEnum_glIsTexture, texture_name, calls_per_chunk, GL_TRUE_VALUE, GLBOOLEAN_RESULT_MASK, &response))
            return 1;
        chunk_mismatches = response.Value[2];
        total_matches += response.Value[1];
        total_mismatches += chunk_mismatches;

        if (chunk_mismatches) {
            const unsigned long first_mismatch_call = response.Value[4];
            const unsigned long mismatch_run_end = first_mismatch_call + chunk_mismatches;
            const int run_reaches_chunk_end = (mismatch_run_end == calls_per_chunk);
            const unsigned long error_after_chunk = settled_gl_error(screen);

            if (chunks_with_mismatch == 0) {
                first_mismatch_value = response.Value[3];
                first_mismatch_chunk = chunk_index;
            }
            chunks_with_mismatch++;
            if (run_reaches_chunk_end)
                runs_reaching_chunk_end++;
            if (error_after_chunk == GL_INVALID_OPERATION_VALUE)
                errors_invalid_operation++;
            else if (error_after_chunk == GL_NO_ERROR_VALUE)
                errors_none++;
            else
                errors_other++;
            if (chunks_with_mismatch <= REPORTED_MISMATCH_CHUNKS)
                report("  Paket %4lu: ab Aufruf %2lu, %3lu abweichend%s, Wert 0x%08lx, glGetError danach 0x%04lx\n", chunk_index, first_mismatch_call, chunk_mismatches, run_reaches_chunk_end ? " bis Paketende" : "", response.Value[3], error_after_chunk);
        } else if ((chunk_index % CONTROL_CHUNK_SPACING) == 0 && controls_done < CONTROL_CHUNK_COUNT) {
            const unsigned long control_error = settled_gl_error(screen);

            controls_done++;
            report("  Kontrolle Paket %4lu ohne Abweichung: glGetError 0x%04lx\n", chunk_index, control_error);
        }
    }
    elapsed_ticks = GetTickCount() - start_ticks;

    report("Ergebnis nach %lu ms, %lu von %lu Paketen gelaufen:\n", (unsigned long)elapsed_ticks, chunk_index, chunk_count);
    report("  TRUE                : %lu\n", total_matches);
    report("  etwas anderes       : %lu  (in %lu Paketen, %lu davon bis zum Paketende)\n", total_mismatches, chunks_with_mismatch, runs_reaching_chunk_end);
    if (total_mismatches) {
        report("  erste Abweichung    : 0x%08lx in Paket %lu\n", first_mismatch_value, first_mismatch_chunk);
        report("  glGetError danach   : GL_INVALID_OPERATION %lu, GL_NO_ERROR %lu, anderes %lu\n", errors_invalid_operation, errors_none, errors_other);
    }

    mark_log(screen, LOG_MARKER_STRESS_END);
    read_state(screen, &response);
    return 0;
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    HDC screen;
    int exit_code = 2;
    int supported;
    const int query_escape = QEMUDISP_ESCAPE_PASSTHROUGH_PROBE;

    report_file = fopen(REPORT_FILE_NAME, "a");

    if (argc < 2) {
        report("Aufruf: PTPROBE phase0 | session | state | istexture <name> <pakete> <je paket>\n");
        return exit_code;
    }

    screen = GetDC(NULL);
    supported = ExtEscape(screen, QUERYESCSUPPORT, sizeof(query_escape), (LPCSTR)&query_escape, 0, NULL);
    report("\n==== PTPROBE %s  (Stempel \"%s\", Escape 0x%x %s)\n", argv[1], QEMU3DFX_COMMIT_STAMP, QEMUDISP_ESCAPE_PASSTHROUGH_PROBE, supported > 0 ? "unterstuetzt" : "NICHT unterstuetzt");

    if (supported <= 0) {
        exit_code = 3;
    } else if (strcmp(argv[1], "phase0") == 0) {
        exit_code = run_phase0(screen);
    } else if (strcmp(argv[1], "session") == 0) {
        exit_code = run_session(screen);
    } else if (strcmp(argv[1], "state") == 0) {
        exit_code = run_state(screen);
    } else if (strcmp(argv[1], "istexture") == 0 && argc >= 5) {
        const unsigned long texture_name = strtoul(argv[2], NULL, 0);
        const unsigned long chunk_count = strtoul(argv[3], NULL, 0);
        const unsigned long calls_per_chunk = strtoul(argv[4], NULL, 0);

        exit_code = run_istexture(screen, texture_name, chunk_count, calls_per_chunk);
    } else {
        report("Unbekannter Befehl: %s\n", argv[1]);
    }

    report("==== Rueckgabewert %d\n", exit_code);
    ReleaseDC(NULL, screen);
    if (report_file != NULL)
        fclose(report_file);
    return exit_code;
}
