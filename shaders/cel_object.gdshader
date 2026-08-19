shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_back;
#include "res://shaders/cel.gdshaderinc"
uniform vec3 base_color_linear=vec3(.45,.42,.40);
uniform vec3 ambient_linear=vec3(.16,.19,.26);
uniform float shadow_visibility:hint_range(0.0,1.0)=1.0;
uniform float ambient_occlusion:hint_range(0.0,1.0)=1.0;
uniform float gloss:hint_range(0.0,1.0)=0.0;
uniform bool probe_mode=false;
uniform vec3 probe_albedo=vec3(.5);
uniform vec3 probe_ambient=vec3(0.0);
uniform float probe_ndl=1.0;
uniform float probe_ndv=1.0;
uniform float probe_ndh=0.0;
uniform float probe_shadow=1.0;
uniform float probe_ao=1.0;
uniform float probe_gloss=0.0;
varying vec3 ve_world_pos;
varying vec3 ve_world_n;
void vertex(){ve_world_pos=(MODEL_MATRIX*vec4(VERTEX,1.0)).xyz;
	ve_world_n=normalize(mat3(MODEL_MATRIX)*NORMAL);}
void fragment(){
	vec3 shaded;
	if(probe_mode){shaded=ve_cel_shade(probe_albedo,probe_ambient,
			probe_ndl,probe_ndv,probe_ndh,probe_shadow,probe_ao,probe_gloss);
	}else{
		vec3 n=normalize(ve_world_n),v=normalize(INV_VIEW_MATRIX[3].xyz-ve_world_pos);
		shaded=ve_cel_shade(base_color_linear,ambient_linear,dot(n,VE_SUN_DIR),dot(n,v),
				dot(n,normalize(VE_SUN_DIR+v)),shadow_visibility,ambient_occlusion,gloss);
	}
	ALBEDO=shaded;}
