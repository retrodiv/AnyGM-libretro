/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Particle-system builtin argument adaptation and ordered dispatch. */
#include "gml_builtin_internal.h"
#include "gml_particle.h"

#include <stdint.h>
#include <string.h>

GmlVal gml_builtin_try_particles(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
    /* ---- particle system (part_type / part_system / part_emitter) ---- */
    if(!strncmp(nm,"part_",5)){
      if(!strcmp(nm,"part_type_create")) return vreal(gml_part_type_create(vm->particles));
      if(!strcmp(nm,"part_type_destroy")){ gml_part_type_destroy(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_type_clear")){ gml_part_type_clear(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_type_exists")) return vreal(gml_part_type_exists(vm->particles,(int)N(a,n,0)));
      if(!strcmp(nm,"part_type_sprite")){ gml_part_type_sprite(vm->particles,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_shape")){ gml_part_type_shape(vm->particles,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_type_size")){ gml_part_type_size(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_scale")){ gml_part_type_scale(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_speed")){ gml_part_type_speed(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_direction")){ gml_part_type_direction(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_type_gravity")){ gml_part_type_gravity(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_life")){ gml_part_type_life(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_orientation")){ gml_part_type_orientation(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),(int)N(a,n,5)); return vreal(0); }
      if(!strcmp(nm,"part_type_step")){ gml_part_type_step(vm->particles,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_death")){ gml_part_type_death(vm->particles,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_color1")||!strcmp(nm,"part_type_colour1")){ gml_part_type_color(vm->particles,(int)N(a,n,0),1,NU32(a,n,1),0,0); return vreal(0); }
      if(!strcmp(nm,"part_type_color2")||!strcmp(nm,"part_type_colour2")){ gml_part_type_color(vm->particles,(int)N(a,n,0),2,NU32(a,n,1),NU32(a,n,2),0); return vreal(0); }
      if(!strcmp(nm,"part_type_color3")||!strcmp(nm,"part_type_colour3")){ gml_part_type_color(vm->particles,(int)N(a,n,0),3,NU32(a,n,1),NU32(a,n,2),NU32(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_rgb")||!strcmp(nm,"part_type_colour_rgb")){ gml_part_type_color_rgb(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_mix")||!strcmp(nm,"part_type_colour_mix")){ gml_part_type_color_mix(vm->particles,(int)N(a,n,0),NU32(a,n,1),NU32(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_type_color_hsv")||!strcmp(nm,"part_type_colour_hsv")){ gml_part_type_color_hsv(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6)); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha1")){ gml_part_type_alpha(vm->particles,(int)N(a,n,0),1,N(a,n,1),0,0); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha2")){ gml_part_type_alpha(vm->particles,(int)N(a,n,0),2,N(a,n,1),N(a,n,2),0); return vreal(0); }
      if(!strcmp(nm,"part_type_alpha3")){ gml_part_type_alpha(vm->particles,(int)N(a,n,0),3,N(a,n,1),N(a,n,2),N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_type_blend")){ gml_part_type_blend(vm->particles,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_create")||!strcmp(nm,"part_system_create_layer")){ return vreal(gml_part_system_create(vm->particles)); }
      if(!strcmp(nm,"part_system_destroy")){ gml_part_system_destroy(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_exists")) return vreal(gml_part_system_exists(vm->particles,(int)N(a,n,0)));
      if(!strcmp(nm,"part_system_clear")){ gml_part_system_clear(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_position")){ gml_part_system_position(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2)); return vreal(0); }
      if(!strcmp(nm,"part_system_automatic_update")){ gml_part_system_automatic_update(vm->particles,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_automatic_draw")){ gml_part_system_automatic_draw(vm->particles,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_update")){ gml_part_system_update(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_drawit")||!strcmp(nm,"part_system_drawit_ext")){ if(R) gml_part_system_drawit(vm->particles,R,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_system_depth")){ gml_part_system_depth(vm->particles,(int)N(a,n,0),N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_system_layer")){
        GmlRtLayer *l=(n>1 && a[1].t==V_STR && a[1].s) ? gml_rt_layer_find_by_name(vm,a[1].s) : gml_rt_layer_find(vm,(int)N(a,n,1));
        if(l) gml_part_system_depth(vm->particles,(int)N(a,n,0),l->depth);
        return vreal(0);
      }
      if(!strcmp(nm,"part_particles_count")) return vreal(gml_part_system_count(vm->particles,(int)N(a,n,0)));
      if(!strcmp(nm,"part_particles_create")){ gml_part_particles_create(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
      if(!strcmp(nm,"part_particles_create_color")||!strcmp(nm,"part_particles_create_colour")){ gml_part_particles_create_color(vm->particles,(int)N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),NU32(a,n,4),(int)N(a,n,5)); return vreal(0); }
      if(!strcmp(nm,"part_particles_clear")){ gml_part_system_clear(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_create")){ return vreal(gml_part_emitter_create(vm->particles,(int)N(a,n,0))); }
      if(!strcmp(nm,"part_emitter_exists")){ return vreal(gml_part_emitter_exists(vm->particles,(int)N(a,n,0),(int)N(a,n,1))); }
      if(!strcmp(nm,"part_emitter_destroy")){ gml_part_emitter_destroy(vm->particles,(int)N(a,n,1)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_destroy_all")){ gml_part_emitter_destroy_all(vm->particles,(int)N(a,n,0)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_region")){ gml_part_emitter_region(vm->particles,(int)N(a,n,0),(int)N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),(int)N(a,n,6),(int)N(a,n,7)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_burst")){ gml_part_emitter_burst(vm->particles,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_stream")){ gml_part_emitter_stream(vm->particles,(int)N(a,n,0),(int)N(a,n,1),(int)N(a,n,2),(int)N(a,n,3)); return vreal(0); }
      if(!strcmp(nm,"part_emitter_clear")){ gml_part_emitter_clear(vm->particles,(int)N(a,n,0),(int)N(a,n,1)); return vreal(0); }
      if(!strncmp(nm,"part_system_drawit",18) || !strncmp(nm,"part_system_",12) || !strncmp(nm,"part_type_",10) || !strncmp(nm,"part_emitter_",13) || !strncmp(nm,"part_particles_",15))
        return vreal(0);   /* remaining part_* variants: safe no-op */
    }
  return gml_builtin_try_draw(vm,nm,a,n);
}
