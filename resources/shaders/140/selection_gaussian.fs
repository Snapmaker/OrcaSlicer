#version 140

uniform sampler2D source_texture;
uniform vec2 inverse_texture_size;
uniform vec2 blur_direction;
uniform float blur_radius;

in vec2 tex_coord;
out vec4 frag_color;

float gaussianPdf(float x, float sigma)
{
    return 0.39894 * exp(-0.5 * x * x / (sigma * sigma)) / sigma;
}

void main()
{
    if (blur_radius <= 0.0)
    {
        frag_color = vec4(0.0, 0.0, 0.0, texture(source_texture, tex_coord).a);
        return;
    }

    const int maxBlurRadius = 4;
    float sigma = max(blur_radius * 0.5, 0.001);
    float weightSum = gaussianPdf(0.0, sigma);
    float blurred = texture(source_texture, tex_coord).a * weightSum;
    vec2 stepUv = blur_direction * inverse_texture_size * blur_radius / float(maxBlurRadius);
    vec2 sampleOffset = stepUv;

    for (int i = 1; i <= maxBlurRadius; ++i)
    {
        float offset = blur_radius * float(i) / float(maxBlurRadius);
        float weight = gaussianPdf(offset, sigma);
        blurred += (texture(source_texture, tex_coord + sampleOffset).a +
                    texture(source_texture, tex_coord - sampleOffset).a) * weight;
        weightSum += 2.0 * weight;
        sampleOffset += stepUv;
    }

    frag_color = vec4(0.0, 0.0, 0.0, blurred / weightSum);
}
