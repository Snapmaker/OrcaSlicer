#version 140
// Experimental 5x5 first pass. Keep the production refinement as a separate pass.
uniform sampler2D depth_texture;
uniform sampler2D ao_texture;
uniform sampler2D edge_texture;
uniform ivec2 full_size;
out vec4 out_color;

bool inside(ivec2 p) {
    return all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,full_size));
}
float link(ivec2 p,ivec2 step) {
    ivec2 q=p+step;
    if(!inside(p)||!inside(q)) return 0.0;
    vec4 a=texelFetch(edge_texture,p,0), b=texelFetch(edge_texture,q,0);
    if(step.x<0) return min(a.r,b.g);
    if(step.x>0) return min(a.g,b.r);
    if(step.y<0) return min(a.b,b.a);
    return min(a.a,b.b);
}
float path_weight(ivec2 p,ivec2 delta,bool horizontal_first) {
    float w=1.0;
    ivec2 sx=ivec2(delta.x<0?-1:1,0), sy=ivec2(0,delta.y<0?-1:1);
    for(int axis=0;axis<2;++axis) {
        bool horizontal=(axis==0)==horizontal_first;
        ivec2 step=horizontal?sx:sy;
        int count=horizontal?abs(delta.x):abs(delta.y);
        for(int i=0;i<2;++i) {
            if(i>=count) break;
            w*=link(p,step);
            p+=step;
        }
    }
    return w;
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    if(texelFetch(depth_texture,p,0).r>=1.0) { out_color=vec4(1); return; }
    float sum=0.0, total=0.0;
    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
        ivec2 delta=ivec2(x,y), q=p+delta;
        if(!inside(q)) continue;
        float guide=path_weight(p,delta,true);
        // Both L-shaped routes must remain connected. Never jump a blocked edge.
        if(x!=0 && y!=0) guide=min(guide,path_weight(p,delta,false));
        float w=exp(-0.32*float(x*x+y*y))*guide;
        sum+=texelFetch(ao_texture,q,0).r*w;
        total+=w;
    }
    out_color=vec4(sum/max(total,1e-6));
}
