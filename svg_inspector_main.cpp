#define WIN32_LEAN_AND_MEAN
#define NOMINMAX                 // <-- Add this or min will be expanded and conflict with std::min
#define _USE_MATH_DEFINES
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <sstream>
#include <fstream>
#include <iostream>

#include <cairo.h>
#include <cairo-pdf.h>
#include <librsvg/rsvg.h>

#include <algorithm>             // <-- Provides std::min, std::max, std::clamp

// -------------------------------------------------------------
// Constants & Structures
// -------------------------------------------------------------
#define WM_USER_TRIGGER_RENDER (WM_USER + 1)
#define WM_USER_DISPATCH_CMD   (WM_USER + 2)
#define PIPE_NAME L"\\\\.\\pipe\\svgviewer_ipc"

enum PaperSize {
    PAPER_CUSTOM = 0,
    PAPER_A4,       // 210 x 297 mm -> 595.28 x 841.89 pt
    PAPER_A3,       // 297 x 420 mm -> 841.89 x 1190.55 pt
    PAPER_LETTER    // 8.5 x 11 in   -> 612.0  x 792.0 pt
};

struct PaperDimensions {
    double width_pt;
    double height_pt;
};

PaperDimensions GetPaperDimensions(PaperSize size) {
    switch (size) {
        case PAPER_A4:     return { 595.276, 841.890 };
        case PAPER_A3:     return { 841.890, 1190.551 };
        case PAPER_LETTER: return { 612.000, 792.000 };
        default:           return { 800.0, 600.0 };
    }
}

struct ViewState {
    double zoom = 1.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
    double rotation_deg = 0.0; // in degrees
    bool lock_aspect_to_paper = false;
    PaperSize paper_size = PAPER_A4;
    int export_dpi = 300;
};

// -------------------------------------------------------------
// Pure CPU Encoders: BMP & Baseline TIFF
// -------------------------------------------------------------
#pragma pack(push, 1)
struct BMPHeader {
    uint16_t bfType{ 0x4D42 };
    uint32_t bfSize{ 0 };
    uint16_t bfReserved1{ 0 };
    uint16_t bfReserved2{ 0 };
    uint32_t bfOffBits{ 54 };
    uint32_t biSize{ 40 };
    int32_t  biWidth{ 0 };
    int32_t  biHeight{ 0 };
    uint16_t biPlanes{ 1 };
    uint16_t biBitCount{ 32 };
    uint32_t biCompression{ 0 };
    uint32_t biSizeImage{ 0 };
    int32_t  biXPelsPerMeter{ 3780 };
    int32_t  biYPelsPerMeter{ 3780 };
    uint32_t biClrUsed{ 0 };
    uint32_t biClrImportant{ 0 };
};

struct TiffTag {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value_offset;
};
#pragma pack(pop)

bool SaveCairoSurfaceToBMP(cairo_surface_t* surface, const std::wstring& filepath) {
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    BMPHeader hdr;
    hdr.biWidth = width;
    hdr.biHeight = height; // Bottom-up if positive, or -height for top-down
    hdr.biSizeImage = stride * height;
    hdr.bfSize = 54 + hdr.biSizeImage;

    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;

    out.write(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    // Flip rows vertically for standard Windows BMP
    for (int y = height - 1; y >= 0; --y) {
        out.write(reinterpret_cast<char*>(data + y * stride), width * 4);
    }
    return true;
}

bool SaveCairoSurfaceToTIFF(cairo_surface_t* surface, const std::wstring& filepath, int dpi) {
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;

    // Little-endian TIFF header
    uint16_t byteOrder = 0x4949; // "II"
    uint16_t magic = 42;
    uint32_t ifdOffset = 8;
    out.write((char*)&byteOrder, 2);
    out.write((char*)&magic, 2);
    out.write((char*)&ifdOffset, 4);

    uint16_t numEntries = 12;
    out.write((char*)&numEntries, 2);

    uint32_t dataOffset = 8 + 2 + numEntries * 12 + 4 + 16; // IFD + nextIFD(0) + extra data
    uint32_t imageSize = width * height * 4;

uint32_t extraDataOffset = static_cast<uint32_t>(8 + 2 + numEntries * 12 + 4);

std::vector<TiffTag> tags = {
    { 256, 4, 1, static_cast<uint32_t>(width) },
    { 257, 4, 1, static_cast<uint32_t>(height) },
    { 258, 3, 4, extraDataOffset },
    { 259, 3, 1, 1u },
    { 262, 3, 1, 2u },
    { 273, 4, 1, dataOffset },
    { 277, 3, 1, 4u },
    { 278, 4, 1, static_cast<uint32_t>(height) },
    { 279, 4, 1, imageSize },
    { 282, 5, 1, extraDataOffset + 8u },
    { 283, 5, 1, extraDataOffset + 8u },
    { 296, 3, 1, 2u }
};


    for (const auto& tag : tags) {
        out.write((char*)&tag, sizeof(tag));
    }
    uint32_t nextIFD = 0;
    out.write((char*)&nextIFD, 4);

    // Write extra payload (BitsPerSample values: 8, 8, 8, 8)
    uint16_t bps[4] = { 8, 8, 8, 8 };
    out.write((char*)bps, 8);

    // Resolution values (rational numerator / denominator)
    uint32_t res[2] = { (uint32_t)dpi, 1 };
    out.write((char*)res, 8);

    // Reorder Cairo BGRA to TIFF RGBA
    std::vector<unsigned char> rowBuffer(width * 4);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = data + y * stride;
        for (int x = 0; x < width; ++x) {
            rowBuffer[x * 4 + 0] = src[x * 4 + 2]; // R
            rowBuffer[x * 4 + 1] = src[x * 4 + 1]; // G
            rowBuffer[x * 4 + 2] = src[x * 4 + 0]; // B
            rowBuffer[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        out.write((char*)rowBuffer.data(), width * 4);
    }
    return true;
}

// -------------------------------------------------------------
// Application & Rendering Context
// -------------------------------------------------------------
class SVGViewerApp {
public:
    HWND hwnd = nullptr;
    RsvgHandle* svg_handle = nullptr;
    ViewState state;
    
    // Mouse Interaction
    bool is_panning = false;
    POINT last_mouse{};

    // Backbuffer
    cairo_surface_t* backbuffer_surface = nullptr;
    int backbuffer_w = 0;
    int backbuffer_h = 0;

    void LoadSVG(const std::string& filepath) {
        GError* error = nullptr;
        if (svg_handle) {
            g_object_unref(svg_handle);
            svg_handle = nullptr;
        }
        svg_handle = rsvg_handle_new_from_file(filepath.c_str(), &error);
        if (error) {
            g_error_free(error);
            return;
        }
        ResetView();
        Invalidate();
    }

    void ResetView() {
        state.zoom = 1.0;
        state.pan_x = 0.0;
        state.pan_y = 0.0;
        state.rotation_deg = 0.0;
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
    }

    void RenderToCairo(cairo_t* cr, double target_w, double target_h) {
        // Clear background with white
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);

        if (!svg_handle) return;



#if LIBRSVG_MAJOR_VERSION > 2 || (LIBRSVG_MAJOR_VERSION == 2 && LIBRSVG_MINOR_VERSION >= 52)

        double doc_w = 0, doc_h = 0;
        rsvg_handle_get_intrinsic_size_in_pixels(svg_handle, &doc_w, &doc_h);
        if (doc_w <= 0 || doc_h <= 0) { doc_w = 800; doc_h = 600; }

#else
        RsvgDimensionData dim;
        rsvg_handle_get_dimensions(svg_handle, &dim);
        double doc_w = (dim.width > 0) ? static_cast<double>(dim.width) : 800.0;
        double doc_h = (dim.height > 0) ? static_cast<double>(dim.height) : 600.0;

#endif
        cairo_save(cr);

        // Apply Viewport Transformations: Center -> Pan -> Rotate -> Scale
        cairo_translate(cr, target_w / 2.0 + state.pan_x, target_h / 2.0 + state.pan_y);
        cairo_rotate(cr, state.rotation_deg * M_PI / 180.0);
        cairo_scale(cr, state.zoom, state.zoom);
        cairo_translate(cr, -doc_w / 2.0, -doc_h / 2.0);


// Auto-route based on the platform's linked library age
#if LIBRSVG_MAJOR_VERSION > 2 || (LIBRSVG_MAJOR_VERSION == 2 && LIBRSVG_MINOR_VERSION >= 52)
    RsvgRectangle viewport = { 0.0, 0.0, doc_w, doc_h };
    GError* err = nullptr;
    gboolean success = rsvg_handle_render_document(svg_handle, cr, &viewport, &err);
    if (err) g_error_free(err);
    
#else
    double rsvg_w = 0, rsvg_h = 0;
    RsvgDimensionData dimensions;
    rsvg_handle_get_dimensions(svg_handle, &dimensions);
    rsvg_w = dimensions.width;
    rsvg_h = dimensions.height;
    
    if (rsvg_w > 0 && rsvg_h > 0) {
        double scale_x = doc_w / rsvg_w;
        double scale_y = doc_h / rsvg_h;
        double scale = std::min(scale_x, scale_y);
        cairo_scale(cr, scale, scale);
    }
    gboolean success = rsvg_handle_render_cairo(svg_handle, cr);
#endif
        

        cairo_restore(cr);

        // Draw Paper Aspect Frame overlay if locked
        if (state.lock_aspect_to_paper) {
            PaperDimensions pd = GetPaperDimensions(state.paper_size);
            double paper_ratio = pd.width_pt / pd.height_pt;
            double canvas_ratio = target_w / target_h;

            double frame_w = target_w, frame_h = target_h;
            if (canvas_ratio > paper_ratio) {
                frame_w = target_h * paper_ratio;
            } else {
                frame_h = target_w / paper_ratio;
            }
            double fx = (target_w - frame_w) / 2.0;
            double fy = (target_h - frame_h) / 2.0;

            cairo_set_source_rgba(cr, 0.8, 0.2, 0.2, 0.8);
            cairo_set_line_width(cr, 2.0);
            cairo_rectangle(cr, fx, fy, frame_w, frame_h);
            cairo_stroke(cr);
        }
    }

    void Paint(HDC hdc, RECT rc) {
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;

        if (!backbuffer_surface || backbuffer_w != w || backbuffer_h != h) {
            if (backbuffer_surface) cairo_surface_destroy(backbuffer_surface);
            backbuffer_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            backbuffer_w = w;
            backbuffer_h = h;
        }

        cairo_t* cr = cairo_create(backbuffer_surface);
        RenderToCairo(cr, w, h);
        cairo_destroy(cr);

        cairo_surface_flush(backbuffer_surface);
        unsigned char* data = cairo_image_surface_get_data(backbuffer_surface);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetDIBitsToDevice(hdc, 0, 0, w, h, 0, 0, 0, h, data, &bmi, DIB_RGB_COLORS);
    }

    // Export Options
    void ExportRaster(const std::wstring& path, bool is_tiff) {
        PaperDimensions pd = GetPaperDimensions(state.paper_size);
        int out_w = 0, out_h = 0;

        if (state.lock_aspect_to_paper) {
            out_w = static_cast<int>(std::round((pd.width_pt / 72.0) * state.export_dpi));
            out_h = static_cast<int>(std::round((pd.height_pt / 72.0) * state.export_dpi));
        } else {
            out_w = backbuffer_w * (state.export_dpi / 96);
            out_h = backbuffer_h * (state.export_dpi / 96);
        }

        cairo_surface_t* exp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out_w, out_h);
        cairo_t* cr = cairo_create(exp_surface);
        RenderToCairo(cr, out_w, out_h);
        cairo_destroy(cr);
        cairo_surface_flush(exp_surface);

        if (is_tiff) {
            SaveCairoSurfaceToTIFF(exp_surface, path, state.export_dpi);
        } else {
            SaveCairoSurfaceToBMP(exp_surface, path);
        }
        cairo_surface_destroy(exp_surface);
    }

    void ExportPDF(const std::string& path) {
        PaperDimensions pd = GetPaperDimensions(state.paper_size);
        double pdf_w = state.lock_aspect_to_paper ? pd.width_pt : (double)backbuffer_w;
        double pdf_h = state.lock_aspect_to_paper ? pd.height_pt : (double)backbuffer_h;

        cairo_surface_t* pdf_surface = cairo_pdf_surface_create(path.c_str(), pdf_w, pdf_h);
        cairo_t* cr = cairo_create(pdf_surface);
        RenderToCairo(cr, pdf_w, pdf_h);
        cairo_show_page(cr);
        cairo_destroy(cr);
        cairo_surface_flush(pdf_surface);
        cairo_surface_destroy(pdf_surface);
    }
};

static SVGViewerApp g_app;

// -------------------------------------------------------------
// Named Pipe JSON-RPC Server
// -------------------------------------------------------------
std::string ExecuteRPCCommand(const std::string& line) {
    // Minimal JSON parsing without heavy dependencies
    std::istringstream iss(line);
    std::string key;
    
    if (line.find("\"load\"") != std::string::npos) {
        size_t first = line.find_first_of("\"", line.find("\"load\"") + 6);
        size_t last = line.find_first_of("\"", first + 1);
        if (first != std::string::npos && last != std::string::npos) {
            std::string file = line.substr(first + 1, last - first - 1);
            g_app.LoadSVG(file);
            return "{\"status\":\"ok\",\"result\":\"loaded\"}\n";
        }
    } else if (line.find("\"zoom\"") != std::string::npos) {
        float factor = 1.0f;
        sscanf_s(line.c_str(), "%*[^0-9.-]%f", &factor);
        g_app.state.zoom *= factor;
        g_app.Invalidate();
        return "{\"status\":\"ok\",\"zoom\":" + std::to_string(g_app.state.zoom) + "}\n";
    } else if (line.find("\"rotate\"") != std::string::npos) {
        float deg = 0.0f;
        sscanf_s(line.c_str(), "%*[^0-9.-]%f", &deg);
        g_app.state.rotation_deg += deg;
        g_app.Invalidate();
        return "{\"status\":\"ok\",\"rotation\":" + std::to_string(g_app.state.rotation_deg) + "}\n";
    } else if (line.find("\"pan\"") != std::string::npos) {
        float dx = 0, dy = 0;
        size_t idx = line.find("\"pan\"");
        sscanf_s(line.c_str() + idx, "%*[^0-9.-]%f%*[^0-9.-]%f", &dx, &dy);
        g_app.state.pan_x += dx;
        g_app.state.pan_y += dy;
        g_app.Invalidate();
        return "{\"status\":\"ok\"}\n";
    }
    return "{\"status\":\"error\",\"message\":\"unknown command\"}\n";
}

void StartNamedPipeServer(std::atomic<bool>& running) {
    while (running) {
        
HANDLE hPipe = CreateNamedPipeW(
    PIPE_NAME,
    PIPE_ACCESS_DUPLEX,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
    1,                  // nMaxInstances
    4096,               // nOutBufferSize
    4096,               // nInBufferSize
    0,                  // nDefaultTimeOut
    NULL                // lpSecurityAttributes
);

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            char buffer[2048];
            DWORD bytesRead = 0;
            while (running && ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string response = ExecuteRPCCommand(buffer);
                DWORD written = 0;
                WriteFile(hPipe, response.c_str(), static_cast<DWORD>(response.length()), &written, NULL);
            }
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

// -------------------------------------------------------------
// Win32 Window Procedure
// -------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        g_app.Paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        double factor = (delta > 0) ? 1.15 : 0.85;
        g_app.state.zoom *= factor;
        g_app.Invalidate();
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
            g_app.Invalidate();
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_app.is_panning) {
            g_app.is_panning = false;
            ReleaseCapture();
        }
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
            case 'R': // Rotate 90 CW
                g_app.state.rotation_deg = std::fmod(g_app.state.rotation_deg + 90.0, 360.0);
                g_app.Invalidate();
                break;
            case 'P': // Toggle Paper Aspect Lock
                g_app.state.lock_aspect_to_paper = !g_app.state.lock_aspect_to_paper;
                g_app.Invalidate();
                break;
            case '1': g_app.state.paper_size = PAPER_A4; g_app.Invalidate(); break;
            case '2': g_app.state.paper_size = PAPER_A3; g_app.Invalidate(); break;
            case '3': g_app.state.paper_size = PAPER_LETTER; g_app.Invalidate(); break;
            case '4': g_app.state.export_dpi = 150; break;
            case '5': g_app.state.export_dpi = 300; break;
            case '6': g_app.state.export_dpi = 600; break;
            case 'S': // Quick Export to test files
                g_app.ExportRaster(L"output.bmp", false);
                g_app.ExportRaster(L"output.tiff", true);
                g_app.ExportPDF("output.pdf");
                MessageBoxW(hwnd, L"Exported to output.bmp, output.tiff, output.pdf", L"Export Done", MB_OK);
                break;
        }
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
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"SVGViewerCPUClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, 
        L"Pure CPU SVG Viewer (Win32 + Cairo + Librsvg)", 
        WS_OVERLAPPEDWINDOW, 
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, 
        NULL, NULL, hInstance, NULL
    );

    g_app.hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Start background JSON-RPC server thread
    std::atomic<bool> rpc_running(true);
    std::thread rpc_thread(StartNamedPipeServer, std::ref(rpc_running));

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    rpc_running = false;
    // Wake up named pipe connect blocking if needed
    HANDLE hCancel = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hCancel != INVALID_HANDLE_VALUE) CloseHandle(hCancel);

    if (rpc_thread.joinable()) {
        rpc_thread.join();
    }

    return (int)msg.wParam;
}
