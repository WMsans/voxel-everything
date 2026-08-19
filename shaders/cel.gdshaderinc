const vec3 VE_SUN_DIR = vec3(0.5746958, 0.7662610, 0.2873479);
const float VE_HUE = 0.055;
const float VE_SAT = 1.35;
const float VE_SPEC_EDGE = 0.72;
const float VE_SPEC = 0.45;
const float VE_RIM = 0.35;
const float VE_RIM_POWER = 3.0;
float ve_cel_level(float ndl) {
	float v=clamp(ndl,0.0,1.0); if(v>0.66)return 1.0;
	if(v>0.32)return .75; if(v>0.08)return .45; return .18;
}
vec3 ve_rgb_to_hsv(vec3 c) {
	float mx=max(c.r,max(c.g,c.b)),mn=min(c.r,min(c.g,c.b)),d=mx-mn,h=0.0;
	if(d>0.0){if(mx==c.r)h=(c.g-c.b)/d+(c.g<c.b?6.0:0.0);
	else if(mx==c.g)h=(c.b-c.r)/d+2.0;else h=(c.r-c.g)/d+4.0;h/=6.0;}
	return vec3(h,mx>0.0?d/mx:0.0,mx);
}
vec3 ve_hsv_to_rgb(vec3 c) {
	float h=fract(c.x)*6.0,s=clamp(c.y,0.0,1.0),v=c.z,f=h-floor(h);
	int i=int(floor(h))%6;float p=v*(1.0-s),q=v*(1.0-s*f),t=v*(1.0-s*(1.0-f));
	if(i==0)return vec3(v,t,p);if(i==1)return vec3(q,v,p);if(i==2)return vec3(p,v,t);
	if(i==3)return vec3(p,q,v);if(i==4)return vec3(t,p,v);return vec3(v,p,q);
}
vec3 ve_shadow_tint(vec3 albedo,float amount){float k=clamp(amount,0.0,1.0);
	vec3 h=ve_rgb_to_hsv(albedo);h.x=fract(h.x+VE_HUE*k);
	h.y=clamp(h.y*(1.0+(VE_SAT-1.0)*k),0.0,1.0);return ve_hsv_to_rgb(h);}
vec3 ve_cel_shade(vec3 albedo,vec3 ambient,float ndl,float ndv,float ndh,
		float shadow,float ao,float gloss){float sh=clamp(shadow,0.0,1.0);
	float lit=ve_cel_level(ndl)*sh;vec3 tint=ve_shadow_tint(albedo,1.0-lit);
	float spec=(gloss>0.0&&ndh>=VE_SPEC_EDGE)?gloss*VE_SPEC*sh:0.0;
	float rim=VE_RIM*pow(1.0-clamp(ndv,0.0,1.0),VE_RIM_POWER);
	return tint*lit+tint*ambient*clamp(ao,0.0,1.0)+vec3(spec+rim);}
