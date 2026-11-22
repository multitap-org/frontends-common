#include "screen_effect_params.h"

bool screen_effect_params_at_rest(const ScreenEffectParams* params)
{
    return params->zoom <= 1.0f && params->desaturate <= 0.0f && params->flash <= 0.0f && params->bars <= 0.0f;
}

void video_capture_free(VideoCapture* capture)
{
    delete[] capture->pixels;
    *capture = VideoCapture{};
}
