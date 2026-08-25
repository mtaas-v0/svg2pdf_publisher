#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cairo.h>
#include <cairo-pdf.h>
#include <librsvg/rsvg.h>
#include <fontconfig/fontconfig.h>
#include <woff2/decode.h>

std::vector<uint8_t> decode_base64(const std::string& input) {
    static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[b64[i]] = i;

    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (char c : input) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.svg> <output.pdf>\n";
        return 1;
    }

    std::string svg_path = argv[1];
    std::string pdf_path = argv[2];

    std::ifstream file(svg_path);
    if (!file.is_open()) { 
        std::cerr << "Failed to open input file: " << svg_path << "\n";
        return 1; 
    }
    std::string svg_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Capture standard embedded font MIME groups seamlessly 
    std::regex b64_regex(R"(url\s*\(\s*['"]?data:[^;]*;base64,([A-Za-z0-9+/=\s]+)['"]?\s*\))");
    std::smatch match;

    std::string::const_iterator search_start(svg_data.cbegin());
    while (std::regex_search(search_start, svg_data.cend(), match, b64_regex)) {
        std::string b64_payload = match[1].str();
        b64_payload.erase(std::remove_if(b64_payload.begin(), b64_payload.end(), [](unsigned char x){ return std::isspace(x); }), b64_payload.end());

        std::vector<uint8_t> woff2_buffer = decode_base64(b64_payload);
        if (woff2_buffer.empty()) {
            search_start = match.suffix().first;
            continue;
        }

        size_t ttf_size = woff2::ComputeWOFF2FinalSize(woff2_buffer.data(), woff2_buffer.size());
        if (ttf_size > 0) {
            std::vector<uint8_t> ttf_buffer(ttf_size);
            if (woff2::ConvertWOFF2ToTTF(woff2_buffer.data(), woff2_buffer.size(), ttf_buffer.data(), ttf_size)) {
                FcConfig* config = FcConfigGetCurrent();
                if (config) {
                    FcConfigAppFontAddMem(config, ttf_buffer.data(), static_cast<int>(ttf_size), FcTrue);
                }
            }
        }
        search_start = match.suffix().first;
    }

    GError* error = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(svg_data.c_str()), svg_data.length(), &error);
    if (error) {
        std::cerr << "SVG Parse Error: " << error->message << "\n";
        g_error_free(error);
        return 1;
    }

    double letter_width = 612.0;   // 8.5" x 72 PostScript points
    double letter_height = 792.0;  // 11.0" x 72 PostScript points

    cairo_surface_t* surface = cairo_pdf_surface_create(pdf_path.c_str(), letter_width, letter_height);
    cairo_t* cr = cairo_create(surface);

    RsvgRectangle viewport = { 0.0, 0.0, letter_width, letter_height };
    
// Auto-route based on the platform's linked library age
#if LIBRSVG_MAJOR_VERSION > 2 || (LIBRSVG_MAJOR_VERSION == 2 && LIBRSVG_MINOR_VERSION >= 52)
    gboolean success = rsvg_handle_render_document(handle, cr, &viewport, &error);
#else
    double rsvg_w = 0, rsvg_h = 0;
    RsvgDimensionData dimensions;
    rsvg_handle_get_dimensions(handle, &dimensions);
    rsvg_w = dimensions.width;
    rsvg_h = dimensions.height;
    
    if (rsvg_w > 0 && rsvg_h > 0) {
        double scale_x = letter_width / rsvg_w;
        double scale_y = letter_height / rsvg_h;
        double scale = std::min(scale_x, scale_y);
        cairo_scale(cr, scale, scale);
    }
    gboolean success = rsvg_handle_render_cairo(handle, cr);
#endif

    if (!success && error) {
        std::cerr << "Rendering failed: " << error->message << "\n";
    }

    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(handle);

    return success ? 0 : 1;
}
