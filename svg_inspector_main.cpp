#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include <cairo.h>
#include <cairo-pdf.h>
#include <librsvg/rsvg.h>
#include <cjson/cJSON.h>

// -------------------------------------------------------------
// Constants & UI Identifiers
// -------------------------------------------------------------
#define MAX_LAYERS 64
#define MAX_HITTESTS 128
#define MAX_ANNOTATIONS 256
#define MAX_EVENT_QUEUE 128
#define DEFAULT_PIPE_NAME L"\\\\.\\pipe\\svgviewer_ipc"

// Menu Command IDs
#define IDM_FILE_OPEN             1001
#define IDM_FILE_EXPORT_BMP       1002
#define IDM_FILE_EXPORT_TIFF      1003
#define IDM_FILE_EXPORT_PDF       1004
#define IDM_FILE_EXIT             1005

#define IDM_VIEW_ZOOM_IN          1101
#define IDM_VIEW_ZOOM_OUT         1102
#define IDM_VIEW_RESET            1103
#define IDM_VIEW_ROTATE_CW        1104
#define IDM_VIEW_ROTATE_CCW       1105
#define IDM_VIEW_TOGGLE_ASPECT    1106
#define IDM_VIEW_PAPER_A4         1107
#define IDM_VIEW_PAPER_A3         1108
#define IDM_VIEW_PAPER_LETTER     1109
#define IDM_VIEW_DPI_150          1110
#define IDM_VIEW_DPI_300          1111
#define IDM_VIEW_DPI_600          1112

#define IDM_LAYER_ATTACH_FILE     1201
#define IDM_LAYER_CLEAR_ALL       1202

#define IDM_ANNOT_ADD_POINT       1301
#define IDM_ANNOT_ADD_RECT        1302
#define IDM_ANNOT_ADD_POLY        1303
#define IDM_ANNOT_ADD_ARROW       1304
#define IDM_ANNOT_SAVE            1305
#define IDM_ANNOT_LOAD            1306
#define IDM_ANNOT_CLEAR           1307

#define IDM_PIPE_TOGGLE           1401
#define IDM_PIPE_RENAME           1402

// Dynamic Context Menu IDs
#define IDM_CTX_ATTACH_HERE       2001
#define IDM_CTX_ANNOT_POINT_HERE  2002
#define IDM_CTX_ANNOT_RECT_HERE   2003
#define IDM_CTX_ANNOT_ARROW_HERE  2004
#define IDM_CTX_HITTEST_BASE      3000

// -------------------------------------------------------------
// Data Structures
// -------------------------------------------------------------
typedef enum {
    PAPER_CUSTOM = 0,
    PAPER_A4,
    PAPER_A3,
    PAPER_LETTER
} PaperSize;

typedef struct {
    double width_pt;
    double height_pt;
} PaperDimensions;


static PaperDimensions GetPaperDimensions(PaperSize size) {
    PaperDimensions pd;
    switch (size) {
        case PAPER_A4:
            pd.width_pt = 595.276;
            pd.height_pt = 841.890;
            break;
        case PAPER_A3:
            pd.width_pt = 841.890;
            pd.height_pt = 1190.551;
            break;
        case PAPER_LETTER:
            pd.width_pt = 612.000;
            pd.height_pt = 792.000;
            break;
        default:
            pd.width_pt = 800.0;
            pd.height_pt = 600.0;
            break;
    }
    return pd;
}

typedef struct {
    double zoom;
    double pan_x;
    double pan_y;
    double rotation_deg;
    bool lock_aspect_to_paper;
    PaperSize paper_size;
    int export_dpi;
} ViewState;

typedef struct {
    char svguid[64];
    char filepath[MAX_PATH];
    RsvgHandle* handle;
    double x, y;
    double scale;
    double rotation_deg;
    double intrinsic_w, intrinsic_h;
} SvgLayer;

typedef struct {
    char hittest_uid[64];
    char svguid[64];
    double x, y, w, h;
    char commands[8][64];
    int num_commands;
} HitTestArea;

typedef struct {
    char hittest_uid[64];
    char command[64];
} ContextMenuEvent;

typedef enum {
    ANNOT_POINT_TEXT = 0,
    ANNOT_RECT_TEXT,
    ANNOT_POLYGON,
    ANNOT_ARROW
} AnnotType;

typedef struct {
    double x, y;
} Point2D;

typedef struct {
    char id[64];
    AnnotType type;
    char text[256];
    double x, y, w, h;
    double arrow_tip_x, arrow_tip_y;
    Point2D poly_points[32];
    int num_points;
} Annotation;

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMPHeader;

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value_offset;
} TiffTag;
#pragma pack(pop)

typedef struct {
    HWND hwnd;
    ViewState state;
    CRITICAL_SECTION cs;

    // Multi-SVG Layers
    SvgLayer layers[MAX_LAYERS];
    int num_layers;
    char root_svg_path[MAX_PATH];

    // Annotations
    Annotation annotations[MAX_ANNOTATIONS];
    int num_annotations;

    // Hit Testing & Context Menu Events
    HitTestArea hit_areas[MAX_HITTESTS];
    int num_hit_areas;
    ContextMenuEvent event_queue[MAX_EVENT_QUEUE];
    int num_events;

    // Fast-Pan Interaction Cache & CPU Buffers
    bool is_panning;
    POINT last_mouse;
    cairo_surface_t* backbuffer_surface;
    cairo_surface_t* cached_surface;
    int backbuffer_w;
    int backbuffer_h;
    bool cache_dirty;
    double cached_pan_x;
    double cached_pan_y;

    // Named Pipe IPC Server
    wchar_t pipe_name[MAX_PATH];
    volatile LONG pipe_enabled;
    HANDLE h_pipe_thread;
    volatile LONG rpc_running;
} AppState;

static AppState g_app;

// -------------------------------------------------------------
// Coordinate Space Transformations
// -------------------------------------------------------------
void ScreenToWorld(double sx, double sy, double* wx, double* wy) {
    double cx = (g_app.backbuffer_w / 2.0) + g_app.state.pan_x;
    double cy = (g_app.backbuffer_h / 2.0) + g_app.state.pan_y;
    double dx = sx - cx;
    double dy = sy - cy;
    double rad = -g_app.state.rotation_deg * M_PI / 180.0;
    double rx = dx * cos(rad) - dy * sin(rad);
    double ry = dx * sin(rad) + dy * cos(rad);
    *wx = rx / g_app.state.zoom;
    *wy = ry / g_app.state.zoom;
}

// -------------------------------------------------------------
// Pure CPU Exporters (BMP, TIFF, PDF)
// -------------------------------------------------------------
bool SaveCairoSurfaceToBMP(cairo_surface_t* surface, const wchar_t* filepath) {
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    BMPHeader hdr = { 0 };
    hdr.bfType = 0x4D42;
    hdr.bfOffBits = sizeof(BMPHeader);
    hdr.biSize = 40;
    hdr.biWidth = width;
    hdr.biHeight = height;
    hdr.biPlanes = 1;
    hdr.biBitCount = 32;
    hdr.biSizeImage = stride * height;
    hdr.bfSize = hdr.bfOffBits + hdr.biSizeImage;

    FILE* f = _wfopen(filepath, L"wb");
    if (!f) return false;

    fwrite(&hdr, sizeof(hdr), 1, f);
    for (int y = height - 1; y >= 0; --y) {
        fwrite(data + y * stride, 1, width * 4, f);
    }
    fclose(f);
    return true;
}

bool SaveCairoSurfaceToTIFF(cairo_surface_t* surface, const wchar_t* filepath, int dpi) {
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    FILE* f = _wfopen(filepath, L"wb");
    if (!f) return false;

    uint16_t byteOrder = 0x4949; // "II"
    uint16_t magic = 42;
    uint32_t ifdOffset = 8;
    fwrite(&byteOrder, 2, 1, f);
    fwrite(&magic, 2, 1, f);
    fwrite(&ifdOffset, 4, 1, f);

    uint16_t numEntries = 12;
    fwrite(&numEntries, 2, 1, f);

    uint32_t extraDataOffset = (uint32_t)(8 + 2 + numEntries * 12 + 4);
    uint32_t dataOffset = extraDataOffset + 16;
    uint32_t imageSize = (uint32_t)(width * height * 4);

    TiffTag tags[12] = {
        { 256, 4, 1, (uint32_t)width },
        { 257, 4, 1, (uint32_t)height },
        { 258, 3, 4, extraDataOffset },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 1, dataOffset },
        { 277, 3, 1, 4 },
        { 278, 4, 1, (uint32_t)height },
        { 279, 4, 1, imageSize },
        { 282, 5, 1, extraDataOffset + 8 },
        { 283, 5, 1, extraDataOffset + 8 },
        { 296, 3, 1, 2 }
    };

    fwrite(tags, sizeof(TiffTag), 12, f);
    uint32_t nextIFD = 0;
    fwrite(&nextIFD, 4, 1, f);

    uint16_t bps[4] = { 8, 8, 8, 8 };
    fwrite(bps, 2, 4, f);
    uint32_t res[2] = { (uint32_t)dpi, 1 };
    fwrite(res, 4, 2, f);

    unsigned char* row = (unsigned char*)malloc(width * 4);
    for (int y = 0; y < height; ++y) {
        unsigned char* src = data + y * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // R
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // B
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        fwrite(row, 1, width * 4, f);
    }
    free(row);
    fclose(f);
    return true;
}

// -------------------------------------------------------------
// Multi-Layer SVG Engine & Vector Drawing
// -------------------------------------------------------------
void InvalidateViewer(bool force_dirty) {
    if (force_dirty) g_app.cache_dirty = true;
    if (g_app.hwnd) RedrawWindow(g_app.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

void RemoveSvgInternal(const char* svguid) {
    for (int i = 0; i < g_app.num_layers; ++i) {
        if (strcmp(g_app.layers[i].svguid, svguid) == 0) {
            if (g_app.layers[i].handle) g_object_unref(g_app.layers[i].handle);
            for (int j = i; j < g_app.num_layers - 1; ++j) {
                g_app.layers[j] = g_app.layers[j + 1];
            }
            g_app.num_layers--;
            break;
        }
    }
}

bool AttachSvgData(const char* svguid, const char* data, size_t len, double x, double y, double scale, double rot) {
    if (g_app.num_layers >= MAX_LAYERS) return false;
    GError* error = NULL;
    RsvgHandle* h = rsvg_handle_new_from_data((const guint8*)data, len, &error);
    if (!h || error) {
        if (error) g_error_free(error);
        return false;
    }

    RemoveSvgInternal(svguid);

    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(h, &dim);

    SvgLayer* layer = &g_app.layers[g_app.num_layers++];
    strncpy_s(layer->svguid, sizeof(layer->svguid), svguid, _TRUNCATE);
    layer->filepath[0] = '\0';
    layer->handle = h;
    layer->x = x;
    layer->y = y;
    layer->scale = (scale > 0.0) ? scale : 1.0;
    layer->rotation_deg = rot;
    layer->intrinsic_w = (dim.width > 0) ? dim.width : 100.0;
    layer->intrinsic_h = (dim.height > 0) ? dim.height : 100.0;

    InvalidateViewer(true);
    return true;
}

bool AttachSvgFile(const char* svguid, const char* filepath, double x, double y, double scale, double rot) {
    if (g_app.num_layers >= MAX_LAYERS) return false;
    GError* error = NULL;
    RsvgHandle* h = rsvg_handle_new_from_file(filepath, &error);
    if (!h || error) {
        if (error) g_error_free(error);
        return false;
    }

    RemoveSvgInternal(svguid);

    RsvgDimensionData dim;
    rsvg_handle_get_dimensions(h, &dim);

    SvgLayer* layer = &g_app.layers[g_app.num_layers++];
    strncpy_s(layer->svguid, sizeof(layer->svguid), svguid, _TRUNCATE);
    strncpy_s(layer->filepath, sizeof(layer->filepath), filepath, _TRUNCATE);
    layer->handle = h;
    layer->x = x;
    layer->y = y;
    layer->scale = (scale > 0.0) ? scale : 1.0;
    layer->rotation_deg = rot;
    layer->intrinsic_w = (dim.width > 0) ? dim.width : 100.0;
    layer->intrinsic_h = (dim.height > 0) ? dim.height : 100.0;

    InvalidateViewer(true);
    return true;
}

void DrawArrow(cairo_t* cr, double from_x, double from_y, double to_x, double to_y) {
    cairo_move_to(cr, from_x, from_y);
    cairo_line_to(cr, to_x, to_y);
    cairo_stroke(cr);

    double angle = atan2(to_y - from_y, to_x - from_x);
    double head_len = 12.0;

    cairo_save(cr);
    cairo_translate(cr, to_x, to_y);
    cairo_rotate(cr, angle);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, -head_len, -head_len / 2.0);
    cairo_line_to(cr, -head_len, head_len / 2.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

void RenderVectorCanvas(cairo_t* cr, double target_w, double target_h, double px, double py) {
    // 1. Strict CPU Clipping
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, target_w, target_h);
    cairo_clip(cr);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_save(cr);
    // Viewport Transformation
    cairo_translate(cr, target_w / 2.0 + px, target_h / 2.0 + py);
    cairo_rotate(cr, g_app.state.rotation_deg * M_PI / 180.0);
    cairo_scale(cr, g_app.state.zoom, g_app.state.zoom);

    // 2. Render SVG Layers
    for (int i = 0; i < g_app.num_layers; ++i) {
        SvgLayer* l = &g_app.layers[i];
        if (!l->handle) continue;
        cairo_save(cr);
        cairo_translate(cr, l->x, l->y);
        cairo_rotate(cr, l->rotation_deg * M_PI / 180.0);
        cairo_scale(cr, l->scale, l->scale);
        rsvg_handle_render_cairo(l->handle, cr);
        cairo_restore(cr);
    }

    // 3. Render Annotations
    for (int i = 0; i < g_app.num_annotations; ++i) {
        Annotation* a = &g_app.annotations[i];
        cairo_save(cr);
        cairo_select_font_face(cr, "Segoe UI", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.0);

        if (a->type == ANNOT_POINT_TEXT) {
            cairo_set_source_rgba(cr, 0.9, 0.15, 0.15, 1.0);
            cairo_arc(cr, a->x, a->y, 4.5, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_move_to(cr, a->x + 8.0, a->y + 4.0);
            cairo_show_text(cr, a->text);
        } else if (a->type == ANNOT_RECT_TEXT) {
            cairo_set_source_rgba(cr, 0.2, 0.5, 0.9, 0.2);
            cairo_rectangle(cr, a->x, a->y, a->w, a->h);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.2, 0.5, 0.9, 0.9);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
            cairo_move_to(cr, a->x + 6.0, a->y + 16.0);
            cairo_show_text(cr, a->text);
        } else if (a->type == ANNOT_POLYGON && a->num_points >= 3) {
            cairo_set_source_rgba(cr, 0.15, 0.75, 0.35, 0.2);
            cairo_move_to(cr, a->poly_points[0].x, a->poly_points[0].y);
            for (int p = 1; p < a->num_points; ++p) {
                cairo_line_to(cr, a->poly_points[p].x, a->poly_points[p].y);
            }
            cairo_close_path(cr);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.15, 0.75, 0.35, 0.9);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
            cairo_move_to(cr, a->poly_points[0].x + 6.0, a->poly_points[0].y + 16.0);
            cairo_show_text(cr, a->text);
        } else if (a->type == ANNOT_ARROW) {
            cairo_set_source_rgba(cr, 0.8, 0.2, 0.8, 0.9);
            cairo_set_line_width(cr, 2.0);
            DrawArrow(cr, a->x, a->y, a->arrow_tip_x, a->arrow_tip_y);
            cairo_move_to(cr, a->x - 10.0, a->y - 8.0);
            cairo_show_text(cr, a->text);
        }
        cairo_restore(cr);
    }

    cairo_restore(cr);
    cairo_restore(cr);
}

// -------------------------------------------------------------
// Sidecar Annotation Persistence using cJSON (-svgAnnotV0.json)
// -------------------------------------------------------------
void GetSidecarAnnotationPath(const char* svg_path, char* out_annot_path, size_t max_len) {
    strncpy_s(out_annot_path, max_len, svg_path, _TRUNCATE);
    char* dot = strrchr(out_annot_path, '.');
    if (dot) *dot = '\0';
    strncat_s(out_annot_path, max_len, "-svgAnnotV0.json", _TRUNCATE);
}

bool SaveAnnotationsJSON(const char* filepath) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", "svgAnnotV0");

    cJSON* arr = cJSON_AddArrayToObject(root, "annotations");
    for (int i = 0; i < g_app.num_annotations; ++i) {
        Annotation* a = &g_app.annotations[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", a->id);
        cJSON_AddNumberToObject(obj, "type", (int)a->type);
        cJSON_AddStringToObject(obj, "text", a->text);
        cJSON_AddNumberToObject(obj, "x", a->x);
        cJSON_AddNumberToObject(obj, "y", a->y);
        cJSON_AddNumberToObject(obj, "w", a->w);
        cJSON_AddNumberToObject(obj, "h", a->h);
        cJSON_AddNumberToObject(obj, "arrow_tip_x", a->arrow_tip_x);
        cJSON_AddNumberToObject(obj, "arrow_tip_y", a->arrow_tip_y);

        if (a->type == ANNOT_POLYGON) {
            cJSON* pts = cJSON_AddArrayToObject(obj, "points");
            for (int p = 0; p < a->num_points; ++p) {
                cJSON* pt = cJSON_CreateArray();
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(a->poly_points[p].x));
                cJSON_AddItemToArray(pt, cJSON_CreateNumber(a->poly_points[p].y));
                cJSON_AddItemToArray(pts, pt);
            }
        }
        cJSON_AddItemToArray(arr, obj);
    }

    char* rendered = cJSON_Print(root);
    FILE* f = fopen(filepath, "w");
    bool ok = false;
    if (f) {
        fputs(rendered, f);
        fclose(f);
        ok = true;
    }
    free(rendered);
    cJSON_Delete(root);
    return ok;
}

bool LoadAnnotationsJSON(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) { fclose(f); return false; }
    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return false;

    cJSON* arr = cJSON_GetObjectItem(root, "annotations");
    if (cJSON_IsArray(arr)) {
        g_app.num_annotations = 0;
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, arr) {
            if (g_app.num_annotations >= MAX_ANNOTATIONS) break;
            Annotation* a = &g_app.annotations[g_app.num_annotations++];
            memset(a, 0, sizeof(Annotation));

            strncpy_s(a->id, sizeof(a->id), cJSON_GetStringValue(cJSON_GetObjectItem(item, "id")), _TRUNCATE);
            a->type = (AnnotType)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "type"));
            strncpy_s(a->text, sizeof(a->text), cJSON_GetStringValue(cJSON_GetObjectItem(item, "text")), _TRUNCATE);
            a->x = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "x"));
            a->y = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "y"));
            a->w = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "w"));
            a->h = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "h"));
            a->arrow_tip_x = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "arrow_tip_x"));
            a->arrow_tip_y = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "arrow_tip_y"));

            cJSON* pts = cJSON_GetObjectItem(item, "points");
            if (cJSON_IsArray(pts)) {
                cJSON* p = NULL;
                a->num_points = 0;
                cJSON_ArrayForEach(p, pts) {
                    if (a->num_points < 32 && cJSON_IsArray(p) && cJSON_GetArraySize(p) >= 2) {
                        a->poly_points[a->num_points].x = cJSON_GetArrayItem(p, 0)->valuedouble;
                        a->poly_points[a->num_points].y = cJSON_GetArrayItem(p, 1)->valuedouble;
                        a->num_points++;
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    InvalidateViewer(true);
    return true;
}

// -------------------------------------------------------------
// Exporters Trigger
// -------------------------------------------------------------
void TriggerExport(const wchar_t* path, int fmt) {
    PaperDimensions pd = GetPaperDimensions(g_app.state.paper_size);
    int out_w = 0, out_h = 0;

    if (g_app.state.lock_aspect_to_paper) {
        out_w = (int)round((pd.width_pt / 72.0) * g_app.state.export_dpi);
        out_h = (int)round((pd.height_pt / 72.0) * g_app.state.export_dpi);
    } else {
        out_w = g_app.backbuffer_w * (g_app.state.export_dpi / 96);
        out_h = g_app.backbuffer_h * (g_app.state.export_dpi / 96);
    }

    if (fmt == IDM_FILE_EXPORT_PDF) {
        char mbPath[MAX_PATH];
        wcstombs(mbPath, path, MAX_PATH);
        double pdf_w = g_app.state.lock_aspect_to_paper ? pd.width_pt : (double)g_app.backbuffer_w;
        double pdf_h = g_app.state.lock_aspect_to_paper ? pd.height_pt : (double)g_app.backbuffer_h;
        cairo_surface_t* pdf_surface = cairo_pdf_surface_create(mbPath, pdf_w, pdf_h);
        cairo_t* cr = cairo_create(pdf_surface);
        RenderVectorCanvas(cr, pdf_w, pdf_h, g_app.state.pan_x, g_app.state.pan_y);
        cairo_show_page(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(pdf_surface);
    } else {
        cairo_surface_t* exp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out_w, out_h);
        cairo_t* cr = cairo_create(exp_surface);
        RenderVectorCanvas(cr, out_w, out_h, g_app.state.pan_x, g_app.state.pan_y);
        cairo_destroy(cr);
        cairo_surface_flush(exp_surface);

        if (fmt == IDM_FILE_EXPORT_BMP) {
            SaveCairoSurfaceToBMP(exp_surface, path);
        } else if (fmt == IDM_FILE_EXPORT_TIFF) {
            SaveCairoSurfaceToTIFF(exp_surface, path, g_app.state.export_dpi);
        }
        cairo_surface_destroy(exp_surface);
    }
}

// -------------------------------------------------------------
// Menus & Context Menu Handling
// -------------------------------------------------------------
void CreateApplicationMenu(HWND hwnd) {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_OPEN, L"&Open Root SVG...\tCtrl+O");
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXPORT_BMP, L"Export to &BMP...");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXPORT_TIFF, L"Export to &TIFF...");
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXPORT_PDF, L"Export to &PDF...");
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit\tAlt+F4");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, L"&File");

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOM_IN, L"Zoom &In (+)");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ZOOM_OUT, L"Zoom &Out (-)");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_RESET, L"&Reset View (0)");
    AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ROTATE_CW, L"Rotate 90° &CW (R)");
    AppendMenuW(hView, MF_STRING, IDM_VIEW_ROTATE_CCW, L"Rotate 90° CC&W");
    AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hView, MF_STRING, IDM_VIEW_TOGGLE_ASPECT, L"Lock Aspect to Paper Size (P)");

    HMENU hPaper = CreatePopupMenu();
    AppendMenuW(hPaper, MF_STRING, IDM_VIEW_PAPER_A4, L"A4 (210 x 297 mm)");
    AppendMenuW(hPaper, MF_STRING, IDM_VIEW_PAPER_A3, L"A3 (297 x 420 mm)");
    AppendMenuW(hPaper, MF_STRING, IDM_VIEW_PAPER_LETTER, L"US Letter (8.5 x 11 in)");
    AppendMenuW(hView, MF_POPUP, (UINT_PTR)hPaper, L"Export Paper Size");

    HMENU hDpi = CreatePopupMenu();
    AppendMenuW(hDpi, MF_STRING, IDM_VIEW_DPI_150, L"150 DPI");
    AppendMenuW(hDpi, MF_STRING, IDM_VIEW_DPI_300, L"300 DPI");
    AppendMenuW(hDpi, MF_STRING, IDM_VIEW_DPI_600, L"600 DPI");
    AppendMenuW(hView, MF_POPUP, (UINT_PTR)hDpi, L"Export DPI");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, L"&View");

    HMENU hLayers = CreatePopupMenu();
    AppendMenuW(hLayers, MF_STRING, IDM_LAYER_ATTACH_FILE, L"&Attach SVG Layer from File...");
    AppendMenuW(hLayers, MF_STRING, IDM_LAYER_CLEAR_ALL, L"&Clear All Overlays");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hLayers, L"&Layers");

    HMENU hAnnot = CreatePopupMenu();
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_ADD_POINT, L"Add Point Marker Text");
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_ADD_RECT, L"Add Box Area Text");
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_ADD_POLY, L"Add Polygon Area");
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_ADD_ARROW, L"Add Arrow Callout");
    AppendMenuW(hAnnot, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_SAVE, L"&Save Sidecar Annotations (-svgAnnotV0.json)");
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_LOAD, L"&Load Sidecar Annotations");
    AppendMenuW(hAnnot, MF_STRING, IDM_ANNOT_CLEAR, L"Clear All Annotations");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hAnnot, L"&Annotations");

    HMENU hPipe = CreatePopupMenu();
    AppendMenuW(hPipe, MF_STRING, IDM_PIPE_TOGGLE, L"&Toggle Named Pipe Server");
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hPipe, L"&IPC Server");

    SetMenu(hwnd, hMenuBar);
}

void HandleContextMenu(HWND hwnd, int mx, int my) {
    EnterCriticalSection(&g_app.cs);
    double wx = 0.0, wy = 0.0;
    ScreenToWorld(mx, my, &wx, &wy);

    // Check Hit Test Areas
    for (int i = g_app.num_hit_areas - 1; i >= 0; --i) {
        HitTestArea* h = &g_app.hit_areas[i];
        double tx = wx, ty = wy;
        if (h->svguid[0] != '\0') {
            for (int l = 0; l < g_app.num_layers; ++l) {
                if (strcmp(g_app.layers[l].svguid, h->svguid) == 0) {
                    tx -= g_app.layers[l].x;
                    ty -= g_app.layers[l].y;
                    break;
                }
            }
        }
        if (tx >= h->x && tx <= (h->x + h->w) && ty >= h->y && ty <= (h->y + h->h)) {
            HMENU hMenu = CreatePopupMenu();
            for (int c = 0; c < h->num_commands; ++c) {
                wchar_t wcmd[64];
                mbstowcs(wcmd, h->commands[c], 64);
                AppendMenuW(hMenu, MF_STRING, IDM_CTX_HITTEST_BASE + c, wcmd);
            }
            POINT pt = { mx, my };
            ClientToScreen(hwnd, &pt);
            LeaveCriticalSection(&g_app.cs);

            int chosen = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (chosen >= IDM_CTX_HITTEST_BASE) {
                EnterCriticalSection(&g_app.cs);
                int idx = chosen - IDM_CTX_HITTEST_BASE;
                if (g_app.num_events < MAX_EVENT_QUEUE) {
                    ContextMenuEvent* ev = &g_app.event_queue[g_app.num_events++];
                    strncpy_s(ev->hittest_uid, sizeof(ev->hittest_uid), h->hittest_uid, _TRUNCATE);
                    strncpy_s(ev->command, sizeof(ev->command), h->commands[idx], _TRUNCATE);
                }
                LeaveCriticalSection(&g_app.cs);
            }
            return;
        }
    }

    // Default Canvas Context Menu
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_ATTACH_HERE, L"Attach SVG File Here...");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_ANNOT_POINT_HERE, L"Add Point Text Here");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_ANNOT_RECT_HERE, L"Add Box Text Here");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_ANNOT_ARROW_HERE, L"Add Arrow Callout Here");

    POINT pt = { mx, my };
    ClientToScreen(hwnd, &pt);
    LeaveCriticalSection(&g_app.cs);

    int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == IDM_CTX_ATTACH_HERE) {
        OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
        wchar_t szFile[MAX_PATH] = { 0 };
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"SVG Files (*.svg)\0*.svg\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            char mbFile[MAX_PATH];
            wcstombs(mbFile, szFile, MAX_PATH);
            char uid[64];
            snprintf(uid, sizeof(uid), "layer_%d", g_app.num_layers + 1);
            AttachSvgFile(uid, mbFile, wx, wy, 1.0, 0.0);
        }
    } else if (cmd == IDM_CTX_ANNOT_POINT_HERE) {
        if (g_app.num_annotations < MAX_ANNOTATIONS) {
            Annotation* a = &g_app.annotations[g_app.num_annotations++];
            snprintf(a->id, sizeof(a->id), "annot_%d", g_app.num_annotations);
            a->type = ANNOT_POINT_TEXT;
            a->x = wx; a->y = wy;
            strncpy_s(a->text, sizeof(a->text), "Point Annotation", _TRUNCATE);
            InvalidateViewer(true);
        }
    } else if (cmd == IDM_CTX_ANNOT_RECT_HERE) {
        if (g_app.num_annotations < MAX_ANNOTATIONS) {
            Annotation* a = &g_app.annotations[g_app.num_annotations++];
            snprintf(a->id, sizeof(a->id), "annot_%d", g_app.num_annotations);
            a->type = ANNOT_RECT_TEXT;
            a->x = wx; a->y = wy; a->w = 120; a->h = 60;
            strncpy_s(a->text, sizeof(a->text), "Box Area", _TRUNCATE);
            InvalidateViewer(true);
        }
    } else if (cmd == IDM_CTX_ANNOT_ARROW_HERE) {
        if (g_app.num_annotations < MAX_ANNOTATIONS) {
            Annotation* a = &g_app.annotations[g_app.num_annotations++];
            snprintf(a->id, sizeof(a->id), "annot_%d", g_app.num_annotations);
            a->type = ANNOT_ARROW;
            a->x = wx; a->y = wy;
            a->arrow_tip_x = wx + 60; a->arrow_tip_y = wy - 60;
            strncpy_s(a->text, sizeof(a->text), "Callout", _TRUNCATE);
            InvalidateViewer(true);
        }
    }
}

// -------------------------------------------------------------
// cJSON-Powered JSON-RPC Protocol Dispatcher
// -------------------------------------------------------------
void ExecuteRPC(const char* raw_json, char* response, size_t max_resp) {
    EnterCriticalSection(&g_app.cs);

    cJSON* root = cJSON_Parse(raw_json);
    if (!root) {
        snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"invalid json\"}\n");
        LeaveCriticalSection(&g_app.cs);
        return;
    }

    cJSON* cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    const char* cmd_name = cJSON_GetStringValue(cmd);

    if (!cmd_name) {
        snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"missing command\"}\n");
    } else if (strcmp(cmd_name, "load_svg_string") == 0) {
        const char* svguid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svguid"));
        const char* svg_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svg_str"));
        double x = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "x"));
        double y = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "y"));
        double scale = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "scale"));
        double rot = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "rotation"));
        if (!svguid) svguid = "layer_default";

        if (svg_str && AttachSvgData(svguid, svg_str, strlen(svg_str), x, y, scale, rot)) {
            snprintf(response, max_resp, "{\"status\":\"ok\",\"svguid\":\"%s\"}\n", svguid);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"failed to parse svg string\"}\n");
        }
    } else if (strcmp(cmd_name, "load_svg_file") == 0) {
        const char* svguid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svguid"));
        const char* filepath = cJSON_GetStringValue(cJSON_GetObjectItem(root, "filepath"));
        double x = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "x"));
        double y = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "y"));
        double scale = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "scale"));
        double rot = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "rotation"));
        if (!svguid) svguid = "root";

        if (filepath && AttachSvgFile(svguid, filepath, x, y, scale, rot)) {
            snprintf(response, max_resp, "{\"status\":\"ok\",\"svguid\":\"%s\"}\n", svguid);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"failed to load file\"}\n");
        }
    } else if (strcmp(cmd_name, "remove_svg") == 0) {
        const char* svguid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svguid"));
        if (svguid) {
            RemoveSvgInternal(svguid);
            InvalidateViewer(true);
            snprintf(response, max_resp, "{\"status\":\"ok\",\"removed\":\"%s\"}\n", svguid);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"missing svguid\"}\n");
        }
    } else if (strcmp(cmd_name, "clear_layers") == 0) {
        for (int i = 0; i < g_app.num_layers; ++i) {
            if (g_app.layers[i].handle) g_object_unref(g_app.layers[i].handle);
        }
        g_app.num_layers = 0;
        InvalidateViewer(true);
        snprintf(response, max_resp, "{\"status\":\"ok\",\"cleared\":true}\n");
    } else if (strcmp(cmd_name, "add_hit_test") == 0) {
        if (g_app.num_hit_areas < MAX_HITTESTS) {
            HitTestArea* h = &g_app.hit_areas[g_app.num_hit_areas++];
            memset(h, 0, sizeof(HitTestArea));

            const char* uid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "hittest_uid"));
            const char* svguid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svguid"));
            if (uid) strncpy_s(h->hittest_uid, sizeof(h->hittest_uid), uid, _TRUNCATE);
            if (svguid) strncpy_s(h->svguid, sizeof(h->svguid), svguid, _TRUNCATE);

            cJSON* rect = cJSON_GetObjectItem(root, "rect");
            if (cJSON_IsArray(rect) && cJSON_GetArraySize(rect) == 4) {
                h->x = cJSON_GetArrayItem(rect, 0)->valuedouble;
                h->y = cJSON_GetArrayItem(rect, 1)->valuedouble;
                h->w = cJSON_GetArrayItem(rect, 2)->valuedouble;
                h->h = cJSON_GetArrayItem(rect, 3)->valuedouble;
            }

            cJSON* cmds = cJSON_GetObjectItem(root, "context_menu_commands");
            if (cJSON_IsArray(cmds)) {
                cJSON* item = NULL;
                h->num_commands = 0;
                cJSON_ArrayForEach(item, cmds) {
                    if (cJSON_IsString(item) && h->num_commands < 8) {
                        strncpy_s(h->commands[h->num_commands++], 64, item->valuestring, _TRUNCATE);
                    }
                }
            }
            snprintf(response, max_resp, "{\"status\":\"ok\",\"hittest_uid\":\"%s\"}\n", h->hittest_uid);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"max hit tests reached\"}\n");
        }
    } else if (strcmp(cmd_name, "drain_context_menu_command_queue") == 0) {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON* events = cJSON_AddArrayToObject(resp, "events");
        for (int i = 0; i < g_app.num_events; ++i) {
            cJSON* ev = cJSON_CreateObject();
            cJSON_AddStringToObject(ev, "hittest_uid", g_app.event_queue[i].hittest_uid);
            cJSON_AddStringToObject(ev, "command", g_app.event_queue[i].command);
            cJSON_AddItemToArray(events, ev);
        }
        g_app.num_events = 0;
        char* printed = cJSON_PrintUnformatted(resp);
        snprintf(response, max_resp, "%s\n", printed);
        free(printed);
        cJSON_Delete(resp);
    } else if (strcmp(cmd_name, "get_rendered_size") == 0) {
        const char* svguid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "svguid"));
        const char* node_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "node_id"));
        bool found = false;

        for (int i = 0; i < g_app.num_layers; ++i) {
            if (svguid && strcmp(g_app.layers[i].svguid, svguid) == 0) {
                RsvgDimensionData dim;
                gboolean ok = FALSE;
                if (node_id && node_id[0] != '\0') {
                    char fmt_id[128];
                    if (node_id[0] != '#') snprintf(fmt_id, sizeof(fmt_id), "#%s", node_id);
                    else strncpy_s(fmt_id, sizeof(fmt_id), node_id, _TRUNCATE);
                    ok = rsvg_handle_get_dimensions_sub(g_app.layers[i].handle, &dim, fmt_id);
                } else {
                    rsvg_handle_get_dimensions(g_app.layers[i].handle, &dim);
                    ok = TRUE;
                }

                if (ok) {
                    snprintf(response, max_resp, "{\"status\":\"ok\",\"svguid\":\"%s\",\"width\":%f,\"height\":%f}\n",
                        svguid, dim.width * g_app.layers[i].scale, dim.height * g_app.layers[i].scale);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"layer or element not found\"}\n");
        }
    } else if (strcmp(cmd_name, "get_viewport") == 0) {
        const char* paper_str = "A4";
        if (g_app.state.paper_size == PAPER_A3) paper_str = "A3";
        else if (g_app.state.paper_size == PAPER_LETTER) paper_str = "Letter";
        else if (g_app.state.paper_size == PAPER_CUSTOM) paper_str = "Custom";

        snprintf(response, max_resp,
            "{\"status\":\"ok\",\"viewport\":{\"zoom\":%f,\"pan_x\":%f,\"pan_y\":%f,\"rotation_deg\":%f,"
            "\"aspect_locked\":%s,\"export_paper_size\":\"%s\",\"export_dpi\":%d,"
            "\"canvas_width\":%d,\"canvas_height\":%d}}\n",
            g_app.state.zoom, g_app.state.pan_x, g_app.state.pan_y, g_app.state.rotation_deg,
            g_app.state.lock_aspect_to_paper ? "true" : "false", paper_str, g_app.state.export_dpi,
            g_app.backbuffer_w, g_app.backbuffer_h);
    } else if (strcmp(cmd_name, "set_viewport") == 0) {
        cJSON* z = cJSON_GetObjectItem(root, "zoom");
        cJSON* px = cJSON_GetObjectItem(root, "pan_x");
        cJSON* py = cJSON_GetObjectItem(root, "pan_y");
        cJSON* rot = cJSON_GetObjectItem(root, "rotation_deg");
        cJSON* dpi = cJSON_GetObjectItem(root, "export_dpi");
        cJSON* aspect = cJSON_GetObjectItem(root, "aspect_locked");

        if (z) g_app.state.zoom = z->valuedouble;
        if (px) g_app.state.pan_x = px->valuedouble;
        if (py) g_app.state.pan_y = py->valuedouble;
        if (rot) g_app.state.rotation_deg = rot->valuedouble;
        if (dpi) g_app.state.export_dpi = (int)dpi->valuedouble;
        if (aspect) g_app.state.lock_aspect_to_paper = cJSON_IsTrue(aspect);

        InvalidateViewer(true);
        snprintf(response, max_resp, "{\"status\":\"ok\"}\n");
    } else if (strcmp(cmd_name, "add_annotation") == 0) {
        if (g_app.num_annotations < MAX_ANNOTATIONS) {
            Annotation* a = &g_app.annotations[g_app.num_annotations++];
            memset(a, 0, sizeof(Annotation));

            const char* uid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
            const char* type_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
            const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));

            if (uid) strncpy_s(a->id, sizeof(a->id), uid, _TRUNCATE);
            if (text) strncpy_s(a->text, sizeof(a->text), text, _TRUNCATE);

            a->x = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "x"));
            a->y = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "y"));
            a->w = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "w"));
            a->h = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "h"));
            a->arrow_tip_x = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "arrow_tip_x"));
            a->arrow_tip_y = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "arrow_tip_y"));

            if (type_str && strcmp(type_str, "rect") == 0) a->type = ANNOT_RECT_TEXT;
            else if (type_str && strcmp(type_str, "arrow") == 0) a->type = ANNOT_ARROW;
            else if (type_str && strcmp(type_str, "polygon") == 0) {
                a->type = ANNOT_POLYGON;
                cJSON* pts = cJSON_GetObjectItem(root, "points");
                if (cJSON_IsArray(pts)) {
                    cJSON* p = NULL;
                    a->num_points = 0;
                    cJSON_ArrayForEach(p, pts) {
                        if (a->num_points < 32 && cJSON_IsArray(p) && cJSON_GetArraySize(p) >= 2) {
                            a->poly_points[a->num_points].x = cJSON_GetArrayItem(p, 0)->valuedouble;
                            a->poly_points[a->num_points].y = cJSON_GetArrayItem(p, 1)->valuedouble;
                            a->num_points++;
                        }
                    }
                }
            } else {
                a->type = ANNOT_POINT_TEXT;
            }
            InvalidateViewer(true);
            snprintf(response, max_resp, "{\"status\":\"ok\",\"annot_id\":\"%s\"}\n", a->id);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"max annotations reached\"}\n");
        }
    } else if (strcmp(cmd_name, "save_annotations") == 0) {
        const char* path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "filepath"));
        char sidecar[MAX_PATH];
        if (!path) {
            GetSidecarAnnotationPath(g_app.root_svg_path, sidecar, sizeof(sidecar));
            path = sidecar;
        }
        if (SaveAnnotationsJSON(path)) {
            snprintf(response, max_resp, "{\"status\":\"ok\",\"path\":\"%s\"}\n", path);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"save failed\"}\n");
        }
    } else if (strcmp(cmd_name, "load_annotations") == 0) {
        const char* path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "filepath"));
        char sidecar[MAX_PATH];
        if (!path) {
            GetSidecarAnnotationPath(g_app.root_svg_path, sidecar, sizeof(sidecar));
            path = sidecar;
        }
        if (LoadAnnotationsJSON(path)) {
            snprintf(response, max_resp, "{\"status\":\"ok\",\"loaded\":%d}\n", g_app.num_annotations);
        } else {
            snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"load failed\"}\n");
        }
    } else {
        snprintf(response, max_resp, "{\"status\":\"error\",\"message\":\"unknown rpc command\"}\n");
    }

    cJSON_Delete(root);
    LeaveCriticalSection(&g_app.cs);
}

// -------------------------------------------------------------
// Named Pipe Worker Thread
// -------------------------------------------------------------
DWORD WINAPI NamedPipeServerThread(LPVOID lpParam) {
    while (InterlockedCompareExchange(&g_app.rpc_running, 1, 1)) {
        if (!InterlockedCompareExchange(&g_app.pipe_enabled, 1, 1)) {
            Sleep(200);
            continue;
        }

        HANDLE hPipe = CreateNamedPipeW(
            g_app.pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && InterlockedCompareExchange(&g_app.rpc_running, 1, 1)) {
            char in_buffer[65536];
            DWORD bytesRead = 0;
            while (ReadFile(hPipe, in_buffer, sizeof(in_buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                in_buffer[bytesRead] = '\0';
                char out_response[65536] = { 0 };
                ExecuteRPC(in_buffer, out_response, sizeof(out_response));
                DWORD written = 0;
                WriteFile(hPipe, out_response, (DWORD)strlen(out_response), &written, NULL);
            }
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
    return 0;
}

// -------------------------------------------------------------
// Win32 Window Procedure
// -------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case IDM_FILE_OPEN: {
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            wchar_t szFile[MAX_PATH] = { 0 };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"SVG Files (*.svg)\0*.svg\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                EnterCriticalSection(&g_app.cs);
                wcstombs(g_app.root_svg_path, szFile, MAX_PATH);
                for (int i = 0; i < g_app.num_layers; ++i) {
                    if (g_app.layers[i].handle) g_object_unref(g_app.layers[i].handle);
                }
                g_app.num_layers = 0;
                AttachSvgFile("root", g_app.root_svg_path, 0, 0, 1.0, 0.0);

                char sidecar[MAX_PATH];
                GetSidecarAnnotationPath(g_app.root_svg_path, sidecar, sizeof(sidecar));
                LoadAnnotationsJSON(sidecar);
                LeaveCriticalSection(&g_app.cs);
            }
            break;
        }
        case IDM_FILE_EXPORT_BMP:
        case IDM_FILE_EXPORT_TIFF:
        case IDM_FILE_EXPORT_PDF: {
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            wchar_t szFile[MAX_PATH] = { 0 };
            ofn.hwndOwner = hwnd;
            if (id == IDM_FILE_EXPORT_BMP) ofn.lpstrFilter = L"BMP Image (*.bmp)\0*.bmp\0";
            else if (id == IDM_FILE_EXPORT_TIFF) ofn.lpstrFilter = L"TIFF Image (*.tiff)\0*.tiff\0";
            else ofn.lpstrFilter = L"PDF Document (*.pdf)\0*.pdf\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            if (GetSaveFileNameW(&ofn)) {
                TriggerExport(szFile, id);
                MessageBoxW(hwnd, L"Export completed successfully.", L"Export", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        case IDM_VIEW_ZOOM_IN:  g_app.state.zoom *= 1.25; InvalidateViewer(true); break;
        case IDM_VIEW_ZOOM_OUT: g_app.state.zoom *= 0.8;  InvalidateViewer(true); break;
        case IDM_VIEW_RESET:    g_app.state.zoom = 1.0; g_app.state.pan_x = 0; g_app.state.pan_y = 0; InvalidateViewer(true); break;
        case IDM_VIEW_ROTATE_CW: g_app.state.rotation_deg = fmod(g_app.state.rotation_deg + 90.0, 360.0); InvalidateViewer(true); break;
        case IDM_VIEW_ROTATE_CCW: g_app.state.rotation_deg = fmod(g_app.state.rotation_deg - 90.0 + 360.0, 360.0); InvalidateViewer(true); break;
        case IDM_VIEW_TOGGLE_ASPECT: g_app.state.lock_aspect_to_paper = !g_app.state.lock_aspect_to_paper; InvalidateViewer(true); break;
        case IDM_VIEW_PAPER_A4: g_app.state.paper_size = PAPER_A4; InvalidateViewer(true); break;
        case IDM_VIEW_PAPER_A3: g_app.state.paper_size = PAPER_A3; InvalidateViewer(true); break;
        case IDM_VIEW_PAPER_LETTER: g_app.state.paper_size = PAPER_LETTER; InvalidateViewer(true); break;
        case IDM_VIEW_DPI_150: g_app.state.export_dpi = 150; break;
        case IDM_VIEW_DPI_300: g_app.state.export_dpi = 300; break;
        case IDM_VIEW_DPI_600: g_app.state.export_dpi = 600; break;
        case IDM_ANNOT_SAVE: {
            char path[MAX_PATH];
            GetSidecarAnnotationPath(g_app.root_svg_path, path, sizeof(path));
            if (SaveAnnotationsJSON(path)) MessageBoxA(hwnd, path, "Saved Sidecar Annotations", MB_OK);
            break;
        }
        case IDM_ANNOT_LOAD: {
            char path[MAX_PATH];
            GetSidecarAnnotationPath(g_app.root_svg_path, path, sizeof(path));
            LoadAnnotationsJSON(path);
            break;
        }
        case IDM_ANNOT_CLEAR: g_app.num_annotations = 0; InvalidateViewer(true); break;
        case IDM_LAYER_CLEAR_ALL: {
            EnterCriticalSection(&g_app.cs);
            for (int i = 1; i < g_app.num_layers; ++i) {
                if (g_app.layers[i].handle) g_object_unref(g_app.layers[i].handle);
            }
            if (g_app.num_layers > 1) g_app.num_layers = 1;
            LeaveCriticalSection(&g_app.cs);
            InvalidateViewer(true);
            break;
        }
        case IDM_PIPE_TOGGLE: {
            LONG active = InterlockedCompareExchange(&g_app.pipe_enabled, 0, 0);
            InterlockedExchange(&g_app.pipe_enabled, !active);
            MessageBoxW(hwnd, !active ? L"Named Pipe Server Enabled." : L"Named Pipe Server Disabled.", L"IPC Server", MB_OK);
            break;
        }
        case IDM_FILE_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        if (w > 0 && h > 0) {
            EnterCriticalSection(&g_app.cs);
            if (!g_app.backbuffer_surface || g_app.backbuffer_w != w || g_app.backbuffer_h != h) {
                if (g_app.backbuffer_surface) cairo_surface_destroy(g_app.backbuffer_surface);
                if (g_app.cached_surface) cairo_surface_destroy(g_app.cached_surface);
                g_app.backbuffer_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                g_app.cached_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                g_app.backbuffer_w = w;
                g_app.backbuffer_h = h;
                g_app.cache_dirty = true;
            }

            if (g_app.cache_dirty) {
                cairo_t* ccr = cairo_create(g_app.cached_surface);
                RenderVectorCanvas(ccr, w, h, g_app.state.pan_x, g_app.state.pan_y);
                cairo_destroy(ccr);
                cairo_surface_flush(g_app.cached_surface);
                g_app.cached_pan_x = g_app.state.pan_x;
                g_app.cached_pan_y = g_app.state.pan_y;
                g_app.cache_dirty = false;
            }

            cairo_t* bcr = cairo_create(g_app.backbuffer_surface);
            cairo_set_source_rgb(bcr, 0.88, 0.88, 0.88);
            cairo_paint(bcr);

            double dx = g_app.state.pan_x - g_app.cached_pan_x;
            double dy = g_app.state.pan_y - g_app.cached_pan_y;
            cairo_set_source_surface(bcr, g_app.cached_surface, dx, dy);
            cairo_paint(bcr);

            if (g_app.state.lock_aspect_to_paper) {
                PaperDimensions pd = GetPaperDimensions(g_app.state.paper_size);
                double paper_ratio = pd.width_pt / pd.height_pt;
                double canvas_ratio = (double)w / (double)h;
                double fw = (canvas_ratio > paper_ratio) ? (h * paper_ratio) : w;
                double fh = (canvas_ratio > paper_ratio) ? h : (w / paper_ratio);
                cairo_set_source_rgba(bcr, 0.85, 0.15, 0.15, 0.85);
                cairo_set_line_width(bcr, 2.0);
                cairo_rectangle(bcr, (w - fw) / 2.0, (h - fh) / 2.0, fw, fh);
                cairo_stroke(bcr);
            }
            cairo_destroy(bcr);
            cairo_surface_flush(g_app.backbuffer_surface);

            BITMAPINFO bmi = { 0 };
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(hdc, 0, 0, w, h, 0, 0, 0, h,
                cairo_image_surface_get_data(g_app.backbuffer_surface), &bmi, DIB_RGB_COLORS);
            LeaveCriticalSection(&g_app.cs);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_app.state.zoom *= (delta > 0) ? 1.15 : 0.85;
        InvalidateViewer(true);
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_app.is_panning = true;
        g_app.last_mouse.x = GET_X_LPARAM(lParam);
        g_app.last_mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g_app.is_panning) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            g_app.state.pan_x += (x - g_app.last_mouse.x);
            g_app.state.pan_y += (y - g_app.last_mouse.y);
            g_app.last_mouse.x = x;
            g_app.last_mouse.y = y;
            InvalidateViewer(false); // Fast paint without vector re-rasterization
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app.is_panning) {
            g_app.is_panning = false;
            ReleaseCapture();
            InvalidateViewer(true); // Crisp vector settle
        }
        return 0;
    case WM_RBUTTONDOWN:
        HandleContextMenu(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -------------------------------------------------------------
// WinMain Entry Point
// -------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    InitializeCriticalSection(&g_app.cs);
    g_app.state.zoom = 1.0;
    g_app.state.export_dpi = 300;
    g_app.state.paper_size = PAPER_A4;
    g_app.cache_dirty = true;
    g_app.pipe_enabled = 1;
    g_app.rpc_running = 1;
    wcsncpy_s(g_app.pipe_name, MAX_PATH, DEFAULT_PIPE_NAME, _TRUNCATE);

    // Support --pipe <name> commandline argument for multi-instance IPC
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pipe") == 0 && i + 1 < argc) {
            if (wcsstr(argv[i + 1], L"\\\\.\\pipe\\") == argv[i + 1]) {
                wcsncpy_s(g_app.pipe_name, MAX_PATH, argv[i + 1], _TRUNCATE);
            } else {
                swprintf_s(g_app.pipe_name, MAX_PATH, L"\\\\.\\pipe\\%s", argv[i + 1]);
            }
            break;
        }
    }
    LocalFree(argv);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SvgViewerFullStandAlone";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"Pure CPU SVG Viewer & Annotator",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        NULL, NULL, hInstance, NULL
    );

    g_app.hwnd = hwnd;
    CreateApplicationMenu(hwnd);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Launch Background JSON-RPC Server Thread
    g_app.h_pipe_thread = CreateThread(NULL, 0, NamedPipeServerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&g_app.rpc_running, 0);
    // Unblock pipe wait
    HANDLE hWake = CreateFileW(g_app.pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hWake != INVALID_HANDLE_VALUE) CloseHandle(hWake);
    WaitForSingleObject(g_app.h_pipe_thread, 1000);
    CloseHandle(g_app.h_pipe_thread);

    DeleteCriticalSection(&g_app.cs);
    return (int)msg.wParam;
}
