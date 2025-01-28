//
// OpenXR API Layer Mirror Capture input plugin for OBS
// by Jabbah https://github.com/Jabbah
//
// This plugin is based on the OpenVR plugin for OBS written
// by Keijo "Kegetys" Ruotsalainen, http://www.kegetys.fi
// https://obsproject.com/forum/resources/openvr-input-plugin.534/
//

//#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "d3d11.lib")

#include <tchar.h>

#define BUF_SIZE 256
WCHAR szName[] = L"OpenXROBSMirrorSurface";
HANDLE hMapFile = NULL;
HANDLE sharedHandle;

#define blog(log_level, message, ...) \
	blog(log_level, "[win_openxr_mirror] " message, ##__VA_ARGS__)

#define debug(message, ...)                                                    \
	blog(LOG_DEBUG, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define info(message, ...)                                                    \
	blog(LOG_INFO, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define warn(message, ...)                 \
	blog(LOG_WARNING, "[%s] " message, \
	     obs_source_get_name(context->source), ##__VA_ARGS__)

struct MirrorSurfaceData {
	uint32_t lastProcessedIndex = 0;
	uint32_t frameNumber = 0;
	uint32_t eyeIndex = -1;
	HANDLE sharedHandle[3] = {NULL};

	void reset()
	{
		for (int i = 0; i < 3; ++i)
			sharedHandle[i] = NULL;
	}
};

MirrorSurfaceData *pMirrorSurfaceData;

struct crop {
	double top;
	double left;
	double bottom;
	double right;
};

struct croppreset {
	char name[128];
	crop crop;
};

std::vector<croppreset> croppresets;

struct win_openxrmirror {
	obs_source_t *source;

	//bool righteye;
	int eyeIndex=-1;
	int croppreset;
	crop crop;

	gs_texture_t *texture = nullptr;
	winrt::com_ptr<ID3D11Device> dev11 = nullptr;
	winrt::com_ptr<ID3D11DeviceContext> ctx11 = nullptr;
	std::vector<winrt::com_ptr<ID3D11Texture2D>> mirror_textures;
	std::vector<winrt::com_ptr<IDXGIResource>> copy_tex_resource_mirrors;

	winrt::com_ptr<ID3D11Texture2D> texCrop = nullptr;

	ULONGLONG lastCheckTick;

	// Set in win_openxrmirror_init, 0 until then.
	unsigned int device_width;
	unsigned int device_height;

	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;

	bool initialized;
	bool active;

	// Set in win_openxrmirror_properties, null until then.
	obs_property_t *crop_left;
	obs_property_t *crop_right;
	obs_property_t *crop_top;
	obs_property_t *crop_bottom;
};

struct DxgiFormatInfo {
	/// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
	/// Both the SRGB and linear formats should be UNORM.
	DXGI_FORMAT srgb, linear, typeless;

	/// THe bits per pixel, bits per channel, and the number of channels
	int bpp, bpc, channels;
};

bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo &out)
{
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels) \
	{                                                        \
		out = DxgiFormatInfo{srgb, linear, typeless,     \
				     bpp,  bpc,    channels};    \
		return true;                                     \
	}

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels)            \
	case name##_TYPELESS:                               \
	case name##_UNORM:                                  \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, \
			     DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels)                                      \
	case name##_TYPELESS:                                                  \
	case name##_UNORM:                                                     \
	case name##_UNORM_SRGB:                                                \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, \
			     bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels)                              \
	case linear:                                                           \
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, \
			     bpp, bpc, channels)

	// Note that this *should* have pretty much all the types we'll ever see in games
	// Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
	// (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
	switch (format) {
		// The relatively traditional 8bpp 32-bit types
		DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

		// Some larger linear-only types
		DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
		DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

		// A jumble of other weird types
		DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
		DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
		DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

	default:
		// Unknown type
		return false;
	}

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
}

// Update the crop sliders with the correct maximum values or hide them if
// we do not know.
static void win_openxrmirror_update_properties(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	if ((context->crop_left && context->crop_right && context->crop_top &&
	     context->crop_bottom)) {
		const bool visible = context->device_width > 0 &&
				     context->device_height > 0;
		obs_property_set_visible(context->crop_left, visible);
		obs_property_set_visible(context->crop_right, visible);
		obs_property_set_visible(context->crop_top, visible);
		obs_property_set_visible(context->crop_bottom, visible);

		obs_property_float_set_limits(context->crop_left, 0,
					    100, 0.1);
		obs_property_float_set_limits(context->crop_right, 0,
					    100, 0.1);
		obs_property_float_set_limits(context->crop_top, 0,
					    100, 0.1);
		obs_property_float_set_limits(context->crop_bottom, 0,
					    100, 0.1);
	}
}

static void win_openxrmirror_deinit(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	context->initialized = false;

	if (context->texture) {
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
		context->texture = NULL;
	}

	if (hMapFile) {
		CloseHandle(hMapFile);
		hMapFile = NULL;
	}

	context->texCrop = nullptr;
	context->mirror_textures.clear();
	context->copy_tex_resource_mirrors.clear();
	context->ctx11 = nullptr;
	context->dev11 = nullptr;

	context->device_width = 0;
	context->device_height = 0;
	context->crop_left = nullptr;
	context->crop_right = nullptr;
	context->crop_top = nullptr;
	context->crop_bottom = nullptr;
}

static void win_openxrmirror_init(void *data, bool forced = false)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (context->initialized)
		return;

	// Dont attempt to init too often
	if (GetTickCount64() - 1000 < context->lastCheckTick && !forced) {
		return;
	}

	// Make sure everything is reset
	win_openxrmirror_deinit(data);

	context->lastCheckTick = GetTickCount64();

	hMapFile = OpenFileMappingW(FILE_MAP_WRITE |
					    FILE_MAP_READ, // read/write access
				    FALSE,   // do not inherit the name
				    szName); // name of mapping object

	if (hMapFile == NULL) {
		warn("win_openxrmirror_init: Could not open file mapping object:  %d",
		     GetLastError());
		return;
	}

	pMirrorSurfaceData = (MirrorSurfaceData *)MapViewOfFile(
		hMapFile,                       // handle to map object
		FILE_MAP_WRITE | FILE_MAP_READ, // read permission
		0, 0, sizeof(MirrorSurfaceData));

	if (pMirrorSurfaceData == nullptr) {
		warn("win_openxrmirror_init: Could not map view of file.");

		CloseHandle(hMapFile);

		return;
	}

	pMirrorSurfaceData->eyeIndex = context->eyeIndex;

	HRESULT hr;
	D3D_FEATURE_LEVEL featureLevel[] = {D3D_FEATURE_LEVEL_11_1,
					    D3D_FEATURE_LEVEL_11_0};
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, 0,
#ifdef _DEBUG
			       D3D11_CREATE_DEVICE_DEBUG |
#endif
				       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			       0, 0, D3D11_SDK_VERSION, context->dev11.put(),
			       featureLevel, context->ctx11.put());
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: D3D11CreateDevice failed");
		return;
	}

	context->mirror_textures = std::vector<winrt::com_ptr<ID3D11Texture2D>>();
	context->copy_tex_resource_mirrors = std::vector<winrt::com_ptr<IDXGIResource>>();

	for (UINT i = 0; i < 3; ++i) {
		sharedHandle = pMirrorSurfaceData->sharedHandle[i];

		if (sharedHandle == NULL) {
			warn("win_openxrmirror_init: Mirror surface handle is null");
			return;
		}

		winrt::com_ptr<IDXGIResource> copy_tex_resource_mirror = nullptr;
		hr = context->dev11->OpenSharedResource(
			sharedHandle, __uuidof(IDXGIResource),
			copy_tex_resource_mirror.put_void());
		if (FAILED(hr) || !copy_tex_resource_mirror) {

			warn("win_openxrmirror_init: OpenSharedResource failed");
			return;
		}
		context->copy_tex_resource_mirrors.push_back(copy_tex_resource_mirror);

		winrt::com_ptr<ID3D11Texture2D> mirror_texture;
		hr = context->copy_tex_resource_mirrors[i]->QueryInterface(
			__uuidof(ID3D11Texture2D),
			mirror_texture.put_void());
		if (FAILED(hr) || !mirror_texture) {
			warn("win_openxrmirror_init: copy_tex_resource_mirror->QueryInterface failed");
			return;
		}
		context->mirror_textures.push_back(mirror_texture);
	}
	sharedHandle = pMirrorSurfaceData->sharedHandle[0];

	D3D11_TEXTURE2D_DESC desc;
	context->mirror_textures[0]->GetDesc(&desc);
	if (desc.Width == 0 || desc.Height == 0) {
		warn("win_openxrmirror_init: device width or height is 0");
		return;
	}
	context->device_width = desc.Width;
	context->device_height = desc.Height;
	win_openxrmirror_update_properties(data);

	// Apply wanted cropping to size
	const crop &crop = context->crop;
	context->x = std::clamp((uint32_t)(crop.left / 100.0 * desc.Width), 0u, desc.Width - 1);
	context->y = std::clamp((uint32_t)(crop.top / 100.0 * desc.Height), 0u, desc.Height - 1);
	const unsigned int remainingWidth = desc.Width - context->x;
	const unsigned int remainingHeight = desc.Height - context->y;
	desc.Width = remainingWidth -
		     std::clamp((uint32_t)(crop.right / 100.0 * remainingWidth), 0u, remainingWidth - 1);
	desc.Height = remainingHeight -
		      std::clamp((uint32_t)(crop.bottom / 100.0 * remainingHeight), 0u, remainingHeight - 1);

	context->width = desc.Width;
	context->height = desc.Height;

	// Create cropped, linear texture
	// Using linear here will cause correct sRGB gamma to be applied
	DxgiFormatInfo info{};
	GetFormatInfo(desc.Format, info);
	desc.Format = info.linear;
	info("Texture format: %d", desc.Format);
	info("Texture width: %d", desc.Width);
	info("Texture height: %d", desc.Height);
	hr = context->dev11->CreateTexture2D(&desc, NULL, context->texCrop.put());
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: CreateTexture2D failed");
		return;
	}

	// Get IDXGIResource, then share handle, and open it in OBS device
	IDXGIResource *res;
	hr = context->texCrop->QueryInterface(__uuidof(IDXGIResource),
					      (void **)&res);
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: QueryInterface failed");
		return;
	}

	HANDLE handle = NULL;
	hr = res->GetSharedHandle(&handle);
	res->Release();
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: GetSharedHandle failed");
		return;
	}

	obs_enter_graphics();
	gs_texture_destroy(context->texture);

#pragma warning(suppress : 4311 4302)
	context->texture = gs_texture_open_shared(reinterpret_cast<uint32_t>(handle));

	obs_leave_graphics();

	context->initialized = true;

}

static const char *win_openxrmirror_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "OpenXR Mirror Capture";
}

static void win_openxrmirror_update(void *data, obs_data_t *settings)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	context->eyeIndex = -1; //obs_data_get_bool(settings, "righteye");

	switch (context->eyeIndex) {
		case 0:
			context->crop.left = obs_data_get_double(settings, "cropright");
			context->crop.right = obs_data_get_double(settings, "cropleft");

		break;
		case 1:
			context->crop.left = obs_data_get_double(settings, "cropleft");
			context->crop.right = obs_data_get_double(settings, "cropright");
		break;
		default:
			context->crop.left = 0;
			context->crop.right = 0;
		break;
	}

	context->crop.top = obs_data_get_double(settings, "croptop");
	context->crop.bottom = obs_data_get_double(settings, "cropbottom");

	if (context->initialized) {
		win_openxrmirror_deinit(data);
		win_openxrmirror_init(data);
	}
}

static void win_openxrmirror_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "righteye", true);
    obs_data_set_default_int(settings, "eyeindex", -1);
	obs_data_set_default_double(settings, "cropleft", 0);
	obs_data_set_default_double(settings, "cropright", 0);
	obs_data_set_default_double(settings, "croptop", 0);
	obs_data_set_default_double(settings, "cropbottom", 0);
}

static uint32_t win_openxrmirror_getwidth(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	return context->width;
}

static uint32_t win_openxrmirror_getheight(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	return context->height;
}

static void win_openxrmirror_show(void *data)
{
	win_openxrmirror_init(data,
		true); // When showing do forced init without delay
}

static void win_openxrmirror_hide(void *data)
{
	win_openxrmirror_deinit(data);
}

static void *win_openxrmirror_create(obs_data_t *settings, obs_source_t *source)
{
	struct win_openxrmirror *context = (win_openxrmirror *)bzalloc(sizeof(win_openxrmirror));
	context->source = source;

	context->initialized = false;

	context->ctx11 = nullptr;
	context->dev11 = nullptr;
	context->texture = nullptr;
	context->texCrop = nullptr;
	context->mirror_textures.clear();
	context->copy_tex_resource_mirrors.clear();

	context->width = context->height = 100;

	win_openxrmirror_update(context, settings);
	return context;
}

static void win_openxrmirror_destroy(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	win_openxrmirror_deinit(data);
	bfree(context);
}

static void win_openxrmirror_render(void *data, gs_effect_t *effect)
{
	if (pMirrorSurfaceData)
		pMirrorSurfaceData->frameNumber++;

	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (context->initialized && pMirrorSurfaceData &&
	    sharedHandle != pMirrorSurfaceData->sharedHandle[0]) {
		win_openxrmirror_deinit(data);
	}

	if (context->active && !context->initialized) {
		// Active & want to render but not initialized - attempt to init
		win_openxrmirror_init(data);
	}

	if (!context->texture || !context->active) {
		return;
	}

	// Crop from full size mirror texture
	// This step is required even without cropping as the full res mirror texture is in sRGB space
	D3D11_BOX poksi = {
		context->x,
		context->y,
		0,
		context->x + context->width,
		context->y + context->height,
		1,
	};

	static uint32_t currFrame = 0;
	uint32_t latestFrame = currFrame;

	if (pMirrorSurfaceData)
		latestFrame = pMirrorSurfaceData->lastProcessedIndex;

	if (currFrame > latestFrame || latestFrame - currFrame > 2) {
		//blog(LOG_INFO, "Resetting currFrame");
		currFrame = latestFrame;
	}

	if (latestFrame - currFrame > 1) {
		//blog(LOG_INFO, "Skipping frame");
		currFrame++;
	}

	context->ctx11->CopySubresourceRegion(context->texCrop.get(), 0, 0, 0, 0,
			context->mirror_textures[0].get(),
					      0, &poksi);
	context->ctx11->Flush();

	currFrame++;

	// Draw from shared mirror texture
	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw")) {
		obs_source_draw(context->texture, 0, 0, 0, 0, false);
	}
}

static void win_openxrmirror_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openxrmirror *context = (win_openxrmirror *)data;

	context->active = obs_source_active(context->source);
}

static bool crop_preset_changed(obs_properties_t *props, obs_property_t *p,
				obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	int sel = (int)obs_data_get_int(s, "croppreset") - 1;

	if (sel >= croppresets.size() || sel < 0)
		return false;

	//bool flip = obs_data_get_bool(s, "righteye");

	// Mirror preset horizontally if right eye is captured
	const crop &crop = croppresets[sel].crop;
	obs_data_set_double(s, "cropleft", std::clamp(crop.left, 0.0, 100.0));
	obs_data_set_double(s, "cropright", std::clamp(crop.right, 0.0, 100.0));
	obs_data_set_double(s, "croptop", std::clamp(crop.top, 0.0, 100.0));
	obs_data_set_double(s, "cropbottom", std::clamp(crop.bottom, 0.0, 100.0));

	return true;
}

static bool crop_preset_manual(obs_properties_t *props, obs_property_t *p,
			       obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	if (obs_data_get_double(s, "croppreset") != 0.0) {
		// Slider moved manually, disable preset
		obs_data_set_double(s, "croppreset", 0);
		return true;
	}
	return false;
}

static bool crop_preset_flip(obs_properties_t *props, obs_property_t *p,
			     obs_data_t *s)
{
	bool flip = obs_data_get_bool(s, "righteye");
	obs_property_set_description(obs_properties_get(props, "cropleft"),
		flip ? obs_module_text("Crop Left Percentage")
		     : obs_module_text("Crop Right Percentage"));
	obs_property_set_description(obs_properties_get(props, "cropright"),
		flip ? obs_module_text("Crop Right Percentage")
		     : obs_module_text("Crop Left Percentage"));
	return true;
}

static bool button_reset_callback(obs_properties_t *props, obs_property_t *p,
				  void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (GetTickCount64() - 2000 < context->lastCheckTick) {
		return false;
	}

	context->lastCheckTick = GetTickCount64();
	context->initialized = false;
	win_openxrmirror_deinit(data);
	return false;
}

static obs_properties_t *win_openxrmirror_properties(void *data)
{
	win_openxrmirror *context = (win_openxrmirror *)data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(props, "righteye",
				    obs_module_text("Right Eye"));
	obs_property_set_modified_callback(p, crop_preset_flip);

	p = obs_properties_add_list(props, "croppreset",
				    obs_module_text("Preset"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "none", 0);
	int i = 1;
	for (const auto &c : croppresets) {
		obs_property_list_add_int(p, c.name, i++);
	}
	obs_property_set_modified_callback(p, crop_preset_changed);

	p = obs_properties_add_float_slider(
		props, "croptop", obs_module_text("Crop Top Percentage"), 0.0, 100.0, 0.1);
	context->crop_top = p;
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropbottom", obs_module_text("Crop Bottom Percentage"), 0.0, 100.0, 0.1);
	context->crop_bottom = p;
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropleft", obs_module_text("Crop Left Percentage"), 0.0, 100.0, 0.1);
	context->crop_left = p;
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropright", obs_module_text("Crop Right Percentage"), 0.0, 100.0, 0.1);
	context->crop_right = p;
	obs_property_set_modified_callback(p, crop_preset_manual);

	p = obs_properties_add_button(props, "resetsteamvr",
				      "Reinitialize OpenXR Mirror Source",
				      button_reset_callback);

	win_openxrmirror_update_properties(data);

	return props;
}

static void load_presets(void)
{
	char *presets_file = NULL;
	presets_file = obs_module_file("win_openxrmirror-presets.ini");
	if (presets_file) {
		FILE *f = fopen(presets_file, "rb");
		if (f) {
			croppreset p = {0};
			while (fscanf(f, "%lf,%lf,%lf,%lf,%[^\n]\n", &p.crop.top,
				      &p.crop.bottom, &p.crop.left,
				      &p.crop.right, p.name) > 0) {
				croppresets.push_back(p);
			}
			fclose(f);
		} else {
			blog(LOG_WARNING,
			     "Failed to load presets file 'win_openxrmirror-presets.ini' not found!");
		}
		bfree(presets_file);
	} else {
		blog(LOG_WARNING,
		     "Failed to load presets file 'win_openxrmirror-presets.ini' not found!");
	}
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win_openxrmirror", "en-US")

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = "openxrmirror_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = win_openxrmirror_get_name;
	info.create = win_openxrmirror_create;
	info.destroy = win_openxrmirror_destroy;
	info.update = win_openxrmirror_update;
	info.get_defaults = win_openxrmirror_defaults;
	info.show = win_openxrmirror_show;
	info.hide = win_openxrmirror_hide;
	info.get_width = win_openxrmirror_getwidth;
	info.get_height = win_openxrmirror_getheight;
	info.video_render = win_openxrmirror_render;
	info.video_tick = win_openxrmirror_tick;
	info.get_properties = win_openxrmirror_properties;
	obs_register_source(&info);
	load_presets();
	return true;
}
