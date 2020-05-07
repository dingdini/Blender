/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* Curve primitive intersection functions. */

#ifdef __HAIR__

#  ifdef __KERNEL_SSE2__
ccl_device_inline ssef transform_point_T3(const ssef t[3], const ssef &a)
{
  return madd(shuffle<0>(a), t[0], madd(shuffle<1>(a), t[1], shuffle<2>(a) * t[2]));
}
#  endif

/* On CPU pass P and dir by reference to aligned vector. */
ccl_device_forceinline bool cardinal_curve_intersect(__thread_space KernelGlobals *kg,
                                                     __thread_space Intersection *isect,
                                                     const float3 ccl_ref P,
                                                     const float3 ccl_ref dir,
                                                     uint visibility,
                                                     int object,
                                                     int curveAddr,
                                                     float time,
                                                     int type)
{
  const bool is_curve_primitive = (type & PRIMITIVE_CURVE);

#  ifndef __KERNEL_OPTIX__ /* see OptiX motion flag OPTIX_MOTION_FLAG_[START|END]_VANISH */
  if (!is_curve_primitive && kernel_data.bvh.use_bvh_steps) {
    const float2 prim_time = kernel_tex_fetch(__prim_time, curveAddr);
    if (time < prim_time.x || time > prim_time.y) {
      return false;
    }
  }
#  endif

  int segment = PRIMITIVE_UNPACK_SEGMENT(type);
  float epsilon = 0.0f;
  float r_st, r_en;

  int depth = kernel_data.curve.subdivisions;
  int flags = kernel_data.curve.curveflags;
  int prim = kernel_tex_fetch(__prim_index, curveAddr);

#  ifdef __KERNEL_SSE2__
  ssef vdir = load4f(dir);
  ssef vcurve_coef[4];
  const float3 *curve_coef = (float3 *)vcurve_coef;

  {
    ssef dtmp = vdir * vdir;
    ssef d_ss = mm_sqrt(dtmp + shuffle<2>(dtmp));
    ssef rd_ss = load1f_first(1.0f) / d_ss;

    ssei v00vec = load4i((ssei *)&kg->__curves.data[prim]);
    int2 &v00 = (int2 &)v00vec;

    int k0 = v00.x + segment;
    int k1 = k0 + 1;
    int ka = max(k0 - 1, v00.x);
    int kb = min(k1 + 1, v00.x + v00.y - 1);

#    if defined(__KERNEL_AVX2__) && defined(__KERNEL_SSE__) && \
        (!defined(_MSC_VER) || _MSC_VER > 1800)
    avxf P_curve_0_1, P_curve_2_3;
    if (is_curve_primitive) {
      P_curve_0_1 = _mm256_loadu2_m128(&kg->__curve_keys.data[k0].x, &kg->__curve_keys.data[ka].x);
      P_curve_2_3 = _mm256_loadu2_m128(&kg->__curve_keys.data[kb].x, &kg->__curve_keys.data[k1].x);
    }
    else {
      int fobject = (object == OBJECT_NONE) ? kernel_tex_fetch(__prim_object, curveAddr) : object;
      motion_cardinal_curve_keys_avx(
          kg, fobject, prim, time, ka, k0, k1, kb, &P_curve_0_1, &P_curve_2_3);
    }
#    else  /* __KERNEL_AVX2__ */
    ssef P_curve[4];

    if (is_curve_primitive) {
      P_curve[0] = load4f(&kg->__curve_keys.data[ka].x);
      P_curve[1] = load4f(&kg->__curve_keys.data[k0].x);
      P_curve[2] = load4f(&kg->__curve_keys.data[k1].x);
      P_curve[3] = load4f(&kg->__curve_keys.data[kb].x);
    }
    else {
      int fobject = (object == OBJECT_NONE) ? kernel_tex_fetch(__prim_object, curveAddr) : object;
      motion_cardinal_curve_keys(kg, fobject, prim, time, ka, k0, k1, kb, (float4 *)&P_curve);
    }
#    endif /* __KERNEL_AVX2__ */

    ssef rd_sgn = set_sign_bit<0, 1, 1, 1>(shuffle<0>(rd_ss));
    ssef mul_zxxy = shuffle<2, 0, 0, 1>(vdir) * rd_sgn;
    ssef mul_yz = shuffle<1, 2, 1, 2>(vdir) * mul_zxxy;
    ssef mul_shuf = shuffle<0, 1, 2, 3>(mul_zxxy, mul_yz);
    ssef vdir0 = vdir & cast(ssei(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0));

    ssef htfm0 = shuffle<0, 2, 0, 3>(mul_shuf, vdir0);
    ssef htfm1 = shuffle<1, 0, 1, 3>(load1f_first(extract<0>(d_ss)), vdir0);
    ssef htfm2 = shuffle<1, 3, 2, 3>(mul_shuf, vdir0);

#    if defined(__KERNEL_AVX2__) && defined(__KERNEL_SSE__) && \
        (!defined(_MSC_VER) || _MSC_VER > 1800)
    const avxf vPP = _mm256_broadcast_ps(&P.m128);
    const avxf htfm00 = avxf(htfm0.m128, htfm0.m128);
    const avxf htfm11 = avxf(htfm1.m128, htfm1.m128);
    const avxf htfm22 = avxf(htfm2.m128, htfm2.m128);

    const avxf p01 = madd(
        shuffle<0>(P_curve_0_1 - vPP),
        htfm00,
        madd(shuffle<1>(P_curve_0_1 - vPP), htfm11, shuffle<2>(P_curve_0_1 - vPP) * htfm22));
    const avxf p23 = madd(
        shuffle<0>(P_curve_2_3 - vPP),
        htfm00,
        madd(shuffle<1>(P_curve_2_3 - vPP), htfm11, shuffle<2>(P_curve_2_3 - vPP) * htfm22));

    const ssef p0 = _mm256_castps256_ps128(p01);
    const ssef p1 = _mm256_extractf128_ps(p01, 1);
    const ssef p2 = _mm256_castps256_ps128(p23);
    const ssef p3 = _mm256_extractf128_ps(p23, 1);

    const ssef P_curve_1 = _mm256_extractf128_ps(P_curve_0_1, 1);
    r_st = ((float4 &)P_curve_1).w;
    const ssef P_curve_2 = _mm256_castps256_ps128(P_curve_2_3);
    r_en = ((float4 &)P_curve_2).w;
#    else  /* __KERNEL_AVX2__ */
    ssef htfm[] = {htfm0, htfm1, htfm2};
    ssef vP = load4f(P);
    ssef p0 = transform_point_T3(htfm, P_curve[0] - vP);
    ssef p1 = transform_point_T3(htfm, P_curve[1] - vP);
    ssef p2 = transform_point_T3(htfm, P_curve[2] - vP);
    ssef p3 = transform_point_T3(htfm, P_curve[3] - vP);

    r_st = ((float4 &)P_curve[1]).w;
    r_en = ((float4 &)P_curve[2]).w;
#    endif /* __KERNEL_AVX2__ */

    float fc = 0.71f;
    ssef vfc = ssef(fc);
    ssef vfcxp3 = vfc * p3;

    vcurve_coef[0] = p1;
    vcurve_coef[1] = vfc * (p2 - p0);
    vcurve_coef[2] = madd(
        ssef(fc * 2.0f), p0, madd(ssef(fc - 3.0f), p1, msub(ssef(3.0f - 2.0f * fc), p2, vfcxp3)));
    vcurve_coef[3] = msub(ssef(fc - 2.0f), p2 - p1, msub(vfc, p0, vfcxp3));
  }
#  else
  float3 curve_coef[4];

  /* curve Intersection check */
  /* obtain curve parameters */
  {
    /* ray transform created - this should be created at beginning of intersection loop */
    Transform htfm;
    float d = sqrtf(dir.x * dir.x + dir.z * dir.z);
    htfm = make_transform(dir.z / d,
                          0,
                          -dir.x / d,
                          0,
                          -dir.x * dir.y / d,
                          d,
                          -dir.y * dir.z / d,
                          0,
                          dir.x,
                          dir.y,
                          dir.z,
                          0);

    float4 v00 = kernel_tex_fetch(__curves, prim);

    int k0 = __float_as_int(v00.x) + segment;
    int k1 = k0 + 1;

    int ka = max(k0 - 1, __float_as_int(v00.x));
    int kb = min(k1 + 1, __float_as_int(v00.x) + __float_as_int(v00.y) - 1);

    float4 P_curve[4];

    if (is_curve_primitive) {
      P_curve[0] = kernel_tex_fetch(__curve_keys, ka);
      P_curve[1] = kernel_tex_fetch(__curve_keys, k0);
      P_curve[2] = kernel_tex_fetch(__curve_keys, k1);
      P_curve[3] = kernel_tex_fetch(__curve_keys, kb);
    }
    else {
      int fobject = (object == OBJECT_NONE) ? kernel_tex_fetch(__prim_object, curveAddr) : object;
      motion_cardinal_curve_keys(kg, fobject, prim, time, ka, k0, k1, kb, P_curve);
    }

    float3 p0 = transform_point(&htfm, float4_to_float3(P_curve[0]) - P);
    float3 p1 = transform_point(&htfm, float4_to_float3(P_curve[1]) - P);
    float3 p2 = transform_point(&htfm, float4_to_float3(P_curve[2]) - P);
    float3 p3 = transform_point(&htfm, float4_to_float3(P_curve[3]) - P);

    float fc = 0.71f;
    curve_coef[0] = p1;
    curve_coef[1] = -fc * p0 + fc * p2;
    curve_coef[2] = 2.0f * fc * p0 + (fc - 3.0f) * p1 + (3.0f - 2.0f * fc) * p2 - fc * p3;
    curve_coef[3] = -fc * p0 + (2.0f - fc) * p1 + (fc - 2.0f) * p2 + fc * p3;
    r_st = P_curve[1].w;
    r_en = P_curve[2].w;
  }
#  endif

  float r_curr = max(r_st, r_en);

  if ((flags & CURVE_KN_RIBBONS) || !(flags & CURVE_KN_BACKFACING))
    epsilon = 2 * r_curr;

  /* find bounds - this is slow for cubic curves */
  float upper, lower;

  float zextrem[4];
  curvebounds(&lower,
              &upper,
              &zextrem[0],
              &zextrem[1],
              &zextrem[2],
              &zextrem[3],
              curve_coef[0].z,
              curve_coef[1].z,
              curve_coef[2].z,
              curve_coef[3].z);
  if (lower - r_curr > isect->t || upper + r_curr < epsilon)
    return false;

  /* minimum width extension */
  float xextrem[4];
  curvebounds(&lower,
              &upper,
              &xextrem[0],
              &xextrem[1],
              &xextrem[2],
              &xextrem[3],
              curve_coef[0].x,
              curve_coef[1].x,
              curve_coef[2].x,
              curve_coef[3].x);
  if (lower > r_curr || upper < -r_curr)
    return false;

  float yextrem[4];
  curvebounds(&lower,
              &upper,
              &yextrem[0],
              &yextrem[1],
              &yextrem[2],
              &yextrem[3],
              curve_coef[0].y,
              curve_coef[1].y,
              curve_coef[2].y,
              curve_coef[3].y);
  if (lower > r_curr || upper < -r_curr)
    return false;

  /* setup recurrent loop */
  int level = 1 << depth;
  int tree = 0;
  float resol = 1.0f / (float)level;
  bool hit = false;

  /* begin loop */
  while (!(tree >> (depth))) {
    const float i_st = tree * resol;
    const float i_en = i_st + (level * resol);

#  ifdef __KERNEL_SSE2__
    ssef vi_st = ssef(i_st), vi_en = ssef(i_en);
    ssef vp_st = madd(madd(madd(vcurve_coef[3], vi_st, vcurve_coef[2]), vi_st, vcurve_coef[1]),
                      vi_st,
                      vcurve_coef[0]);
    ssef vp_en = madd(madd(madd(vcurve_coef[3], vi_en, vcurve_coef[2]), vi_en, vcurve_coef[1]),
                      vi_en,
                      vcurve_coef[0]);

    ssef vbmin = min(vp_st, vp_en);
    ssef vbmax = max(vp_st, vp_en);

    float3 &bmin = (float3 &)vbmin, &bmax = (float3 &)vbmax;
    float &bminx = bmin.x, &bminy = bmin.y, &bminz = bmin.z;
    float &bmaxx = bmax.x, &bmaxy = bmax.y, &bmaxz = bmax.z;
    float3 &p_st = (float3 &)vp_st, &p_en = (float3 &)vp_en;
#  else
    float3 p_st = ((curve_coef[3] * i_st + curve_coef[2]) * i_st + curve_coef[1]) * i_st +
                  curve_coef[0];
    float3 p_en = ((curve_coef[3] * i_en + curve_coef[2]) * i_en + curve_coef[1]) * i_en +
                  curve_coef[0];

    float bminx = min(p_st.x, p_en.x);
    float bmaxx = max(p_st.x, p_en.x);
    float bminy = min(p_st.y, p_en.y);
    float bmaxy = max(p_st.y, p_en.y);
    float bminz = min(p_st.z, p_en.z);
    float bmaxz = max(p_st.z, p_en.z);
#  endif

    if (xextrem[0] >= i_st && xextrem[0] <= i_en) {
      bminx = min(bminx, xextrem[1]);
      bmaxx = max(bmaxx, xextrem[1]);
    }
    if (xextrem[2] >= i_st && xextrem[2] <= i_en) {
      bminx = min(bminx, xextrem[3]);
      bmaxx = max(bmaxx, xextrem[3]);
    }
    if (yextrem[0] >= i_st && yextrem[0] <= i_en) {
      bminy = min(bminy, yextrem[1]);
      bmaxy = max(bmaxy, yextrem[1]);
    }
    if (yextrem[2] >= i_st && yextrem[2] <= i_en) {
      bminy = min(bminy, yextrem[3]);
      bmaxy = max(bmaxy, yextrem[3]);
    }
    if (zextrem[0] >= i_st && zextrem[0] <= i_en) {
      bminz = min(bminz, zextrem[1]);
      bmaxz = max(bmaxz, zextrem[1]);
    }
    if (zextrem[2] >= i_st && zextrem[2] <= i_en) {
      bminz = min(bminz, zextrem[3]);
      bmaxz = max(bmaxz, zextrem[3]);
    }

    float r1 = r_st + (r_en - r_st) * i_st;
    float r2 = r_st + (r_en - r_st) * i_en;
    r_curr = max(r1, r2);

    if (bminz - r_curr > isect->t || bmaxz + r_curr < epsilon || bminx > r_curr ||
        bmaxx < -r_curr || bminy > r_curr || bmaxy < -r_curr) {
      /* the bounding box does not overlap the square centered at O */
      tree += level;
      level = tree & -tree;
    }
    else if (level == 1) {

      /* the maximum recursion depth is reached.
       * check if dP0.(Q-P0)>=0 and dPn.(Pn-Q)>=0.
       * dP* is reversed if necessary.*/
      float t = isect->t;
      float u = 0.0f;
      float gd = 0.0f;

      if (flags & CURVE_KN_RIBBONS) {
        float3 tg = (p_en - p_st);
#  ifdef __KERNEL_SSE__
        const float3 tg_sq = tg * tg;
        float w = tg_sq.x + tg_sq.y;
#  else
        float w = tg.x * tg.x + tg.y * tg.y;
#  endif
        if (w == 0) {
          tree++;
          level = tree & -tree;
          continue;
        }
#  ifdef __KERNEL_SSE__
        const float3 p_sttg = p_st * tg;
        w = -(p_sttg.x + p_sttg.y) / w;
#  else
        w = -(p_st.x * tg.x + p_st.y * tg.y) / w;
#  endif
        w = saturate(w);

        /* compute u on the curve segment */
        u = i_st * (1 - w) + i_en * w;
        r_curr = r_st + (r_en - r_st) * u;
        /* compare x-y distances */
        float3 p_curr = ((curve_coef[3] * u + curve_coef[2]) * u + curve_coef[1]) * u +
                        curve_coef[0];

        float3 dp_st = (3 * curve_coef[3] * i_st + 2 * curve_coef[2]) * i_st + curve_coef[1];
        if (dot(tg, dp_st) < 0)
          dp_st *= -1;
        if (dot(dp_st, -p_st) + p_curr.z * dp_st.z < 0) {
          tree++;
          level = tree & -tree;
          continue;
        }
        float3 dp_en = (3 * curve_coef[3] * i_en + 2 * curve_coef[2]) * i_en + curve_coef[1];
        if (dot(tg, dp_en) < 0)
          dp_en *= -1;
        if (dot(dp_en, p_en) - p_curr.z * dp_en.z < 0) {
          tree++;
          level = tree & -tree;
          continue;
        }

        if (p_curr.x * p_curr.x + p_curr.y * p_curr.y >= r_curr * r_curr || p_curr.z <= epsilon ||
            isect->t < p_curr.z) {
          tree++;
          level = tree & -tree;
          continue;
        }

        t = p_curr.z;
      }
      else {
        float l = len(p_en - p_st);
        float invl = 1.0f / l;
        float3 tg = (p_en - p_st) * invl;
        gd = (r2 - r1) * invl;
        float difz = -dot(p_st, tg);
        float cyla = 1.0f - (tg.z * tg.z * (1 + gd * gd));
        float invcyla = 1.0f / cyla;
        float halfb = (-p_st.z - tg.z * (difz + gd * (difz * gd + r1)));
        float tcentre = -halfb * invcyla;
        float zcentre = difz + (tg.z * tcentre);
        float3 tdif = -p_st;
        tdif.z += tcentre;
        float tdifz = dot(tdif, tg);
        float tb = 2 * (tdif.z - tg.z * (tdifz + gd * (tdifz * gd + r1)));
        float tc = dot(tdif, tdif) - tdifz * tdifz * (1 + gd * gd) - r1 * r1 - 2 * r1 * tdifz * gd;
        float td = tb * tb - 4 * cyla * tc;
        if (td < 0.0f) {
          tree++;
          level = tree & -tree;
          continue;
        }

        float rootd = sqrtf(td);
        float correction = (-tb - rootd) * 0.5f * invcyla;
        t = tcentre + correction;

        float3 dp_st = (3 * curve_coef[3] * i_st + 2 * curve_coef[2]) * i_st + curve_coef[1];
        if (dot(tg, dp_st) < 0)
          dp_st *= -1;
        float3 dp_en = (3 * curve_coef[3] * i_en + 2 * curve_coef[2]) * i_en + curve_coef[1];
        if (dot(tg, dp_en) < 0)
          dp_en *= -1;

        if (flags & CURVE_KN_BACKFACING &&
            (dot(dp_st, -p_st) + t * dp_st.z < 0 || dot(dp_en, p_en) - t * dp_en.z < 0 ||
             isect->t < t || t <= 0.0f)) {
          correction = (-tb + rootd) * 0.5f * invcyla;
          t = tcentre + correction;
        }

        if (dot(dp_st, -p_st) + t * dp_st.z < 0 || dot(dp_en, p_en) - t * dp_en.z < 0 ||
            isect->t < t || t <= 0.0f) {
          tree++;
          level = tree & -tree;
          continue;
        }

        float w = (zcentre + (tg.z * correction)) * invl;
        w = saturate(w);
        /* compute u on the curve segment */
        u = i_st * (1 - w) + i_en * w;
      }
      /* we found a new intersection */

#  ifdef __VISIBILITY_FLAG__
      /* visibility flag test. we do it here under the assumption
       * that most triangles are culled by node flags */
      if (kernel_tex_fetch(__prim_visibility, curveAddr) & visibility)
#  endif
      {
        /* record intersection */
        isect->t = t;
        isect->u = u;
        isect->v = gd;
        isect->prim = curveAddr;
        isect->object = object;
        isect->type = type;
        hit = true;
      }

      tree++;
      level = tree & -tree;
    }
    else {
      /* split the curve into two curves and process */
      level = level >> 1;
    }
  }

  return hit;
}

ccl_device_forceinline bool curve_intersect(__thread_space KernelGlobals *kg,
                                            __thread_space Intersection *isect,
                                            float3 P,
                                            float3 direction,
                                            uint visibility,
                                            int object,
                                            int curveAddr,
                                            float time,
                                            int type)
{
  /* define few macros to minimize code duplication for SSE */
#  ifndef __KERNEL_SSE2__
#    define len3_squared(x) len_squared(x)
#    define len3(x) len(x)
#    define dot3(x, y) dot(x, y)
#  endif

  const bool is_curve_primitive = (type & PRIMITIVE_CURVE);

#  ifndef __KERNEL_OPTIX__ /* see OptiX motion flag OPTIX_MOTION_FLAG_[START|END]_VANISH */
  if (!is_curve_primitive && kernel_data.bvh.use_bvh_steps) {
    const float2 prim_time = kernel_tex_fetch(__prim_time, curveAddr);
    if (time < prim_time.x || time > prim_time.y) {
      return false;
    }
  }
#  endif

  int segment = PRIMITIVE_UNPACK_SEGMENT(type);
  /* curve Intersection check */
  int flags = kernel_data.curve.curveflags;

  int prim = kernel_tex_fetch(__prim_index, curveAddr);
  float4 v00 = kernel_tex_fetch(__curves, prim);

  int cnum = __float_as_int(v00.x);
  int k0 = cnum + segment;
  int k1 = k0 + 1;

#  ifndef __KERNEL_SSE2__
  float4 P_curve[2];

  if (is_curve_primitive) {
    P_curve[0] = kernel_tex_fetch(__curve_keys, k0);
    P_curve[1] = kernel_tex_fetch(__curve_keys, k1);
  }
  else {
    int fobject = (object == OBJECT_NONE) ? kernel_tex_fetch(__prim_object, curveAddr) : object;
    motion_curve_keys(kg, fobject, prim, time, k0, k1, P_curve);
  }

  float r1 = P_curve[0].w;
  float r2 = P_curve[1].w;
  float3 p1 = float4_to_float3(P_curve[0]);
  float3 p2 = float4_to_float3(P_curve[1]);

  /* minimum width extension */
  float3 dif = P - p1;
  float3 dif_second = P - p2;

  float3 p21_diff = p2 - p1;
  float3 sphere_dif1 = (dif + dif_second) * 0.5f;
  float3 dir = direction;
  float sphere_b_tmp = dot3(dir, sphere_dif1);
  float3 sphere_dif2 = sphere_dif1 - sphere_b_tmp * dir;
#  else
  ssef P_curve[2];

  if (is_curve_primitive) {
    P_curve[0] = load4f(&kg->__curve_keys.data[k0].x);
    P_curve[1] = load4f(&kg->__curve_keys.data[k1].x);
  }
  else {
    int fobject = (object == OBJECT_NONE) ? kernel_tex_fetch(__prim_object, curveAddr) : object;
    motion_curve_keys(kg, fobject, prim, time, k0, k1, (float4 *)&P_curve);
  }

  ssef r12 = shuffle<3, 3, 3, 3>(P_curve[0], P_curve[1]);
  const ssef vP = load4f(P);
  const ssef dif = vP - P_curve[0];
  const ssef dif_second = vP - P_curve[1];
  float r1 = extract<0>(r12), r2 = extract<0>(shuffle<2>(r12));

  const ssef p21_diff = P_curve[1] - P_curve[0];
  const ssef sphere_dif1 = (dif + dif_second) * 0.5f;
  const ssef dir = load4f(direction);
  const ssef sphere_b_tmp = dot3_splat(dir, sphere_dif1);
  const ssef sphere_dif2 = nmadd(sphere_b_tmp, dir, sphere_dif1);
#  endif

  float mr = max(r1, r2);
  float l = len3(p21_diff);
  float invl = 1.0f / l;
  float sp_r = mr + 0.5f * l;

  float sphere_b = dot3(dir, sphere_dif2);
  float sdisc = sphere_b * sphere_b - len3_squared(sphere_dif2) + sp_r * sp_r;

  if (sdisc < 0.0f)
    return false;

    /* obtain parameters and test midpoint distance for suitable modes */
#  ifndef __KERNEL_SSE2__
  float3 tg = p21_diff * invl;
#  else
  const ssef tg = p21_diff * invl;
#  endif
  float gd = (r2 - r1) * invl;

  float dirz = dot3(dir, tg);
  float difz = dot3(dif, tg);

  float a = 1.0f - (dirz * dirz * (1 + gd * gd));

  float halfb = dot3(dir, dif) - dirz * (difz + gd * (difz * gd + r1));

  float tcentre = -halfb / a;
  float zcentre = difz + (dirz * tcentre);

  if ((tcentre > isect->t) && !(flags & CURVE_KN_ACCURATE))
    return false;
  if ((zcentre < 0 || zcentre > l) && !(flags & CURVE_KN_ACCURATE) &&
      !(flags & CURVE_KN_INTERSECTCORRECTION))
    return false;

    /* test minimum separation */
#  ifndef __KERNEL_SSE2__
  float3 cprod = cross(tg, dir);
  float cprod2sq = len3_squared(cross(tg, dif));
#  else
  const ssef cprod = cross(tg, dir);
  float cprod2sq = len3_squared(cross_zxy(tg, dif));
#  endif
  float cprodsq = len3_squared(cprod);
  float distscaled = dot3(cprod, dif);

  if (cprodsq == 0)
    distscaled = cprod2sq;
  else
    distscaled = (distscaled * distscaled) / cprodsq;

  if (distscaled > mr * mr)
    return false;

    /* calculate true intersection */
#  ifndef __KERNEL_SSE2__
  float3 tdif = dif + tcentre * dir;
#  else
  const ssef tdif = madd(ssef(tcentre), dir, dif);
#  endif
  float tdifz = dot3(tdif, tg);
  float tdifma = tdifz * gd + r1;
  float tb = 2 * (dot3(dir, tdif) - dirz * (tdifz + gd * tdifma));
  float tc = dot3(tdif, tdif) - tdifz * tdifz - tdifma * tdifma;
  float td = tb * tb - 4 * a * tc;

  if (td < 0.0f)
    return false;

  float rootd = 0.0f;
  float correction = 0.0f;
  if (flags & CURVE_KN_ACCURATE) {
    rootd = sqrtf(td);
    correction = ((-tb - rootd) / (2 * a));
  }

  float t = tcentre + correction;

  if (t < isect->t) {

    if (flags & CURVE_KN_INTERSECTCORRECTION) {
      rootd = sqrtf(td);
      correction = ((-tb - rootd) / (2 * a));
      t = tcentre + correction;
    }

    float z = zcentre + (dirz * correction);
    // bool backface = false;

    if (flags & CURVE_KN_BACKFACING && (t < 0.0f || z < 0 || z > l)) {
      // backface = true;
      correction = ((-tb + rootd) / (2 * a));
      t = tcentre + correction;
      z = zcentre + (dirz * correction);
    }

    if (t > 0.0f && t < isect->t && z >= 0 && z <= l) {

      if (flags & CURVE_KN_ENCLOSEFILTER) {
        float enc_ratio = 1.01f;
        if ((difz > -r1 * enc_ratio) && (dot3(dif_second, tg) < r2 * enc_ratio)) {
          float a2 = 1.0f - (dirz * dirz * (1 + gd * gd * enc_ratio * enc_ratio));
          float c2 = dot3(dif, dif) - difz * difz * (1 + gd * gd * enc_ratio * enc_ratio) -
                     r1 * r1 * enc_ratio * enc_ratio - 2 * r1 * difz * gd * enc_ratio;
          if (a2 * c2 < 0.0f)
            return false;
        }
      }

#  ifdef __VISIBILITY_FLAG__
      /* visibility flag test. we do it here under the assumption
       * that most triangles are culled by node flags */
      if (kernel_tex_fetch(__prim_visibility, curveAddr) & visibility)
#  endif
      {
        /* record intersection */
        isect->t = t;
        isect->u = z * invl;
        isect->v = gd;
        isect->prim = curveAddr;
        isect->object = object;
        isect->type = type;

        return true;
      }
    }
  }

  return false;

#  ifndef __KERNEL_SSE2__
#    undef len3_squared
#    undef len3
#    undef dot3
#  endif
}

ccl_device_inline float3 curve_refine(__thread_space KernelGlobals *kg,
                                      __thread_space ShaderData *sd,
                                      __thread_space const Intersection *isect,
                                      __thread_space const Ray *ray)
{
  int flag = kernel_data.curve.curveflags;
  float t = isect->t;
  float3 P = ray->P;
  float3 D = ray->D;

  if (isect->object != OBJECT_NONE) {
#  ifdef __OBJECT_MOTION__
    Transform tfm = sd->ob_itfm;
#  else
    Transform tfm = object_fetch_transform(kg, isect->object, OBJECT_INVERSE_TRANSFORM);
#  endif

    P = transform_point(&tfm, P);
    D = transform_direction(&tfm, D * t);
    D = normalize_len(D, &t);
  }

  int prim = kernel_tex_fetch(__prim_index, isect->prim);
  float4 v00 = kernel_tex_fetch(__curves, prim);

  int k0 = __float_as_int(v00.x) + PRIMITIVE_UNPACK_SEGMENT(sd->type);
  int k1 = k0 + 1;

  float3 tg;

  if (flag & CURVE_KN_INTERPOLATE) {
    int ka = max(k0 - 1, __float_as_int(v00.x));
    int kb = min(k1 + 1, __float_as_int(v00.x) + __float_as_int(v00.y) - 1);

    float4 P_curve[4];

    if (sd->type & PRIMITIVE_CURVE) {
      P_curve[0] = kernel_tex_fetch(__curve_keys, ka);
      P_curve[1] = kernel_tex_fetch(__curve_keys, k0);
      P_curve[2] = kernel_tex_fetch(__curve_keys, k1);
      P_curve[3] = kernel_tex_fetch(__curve_keys, kb);
    }
    else {
      motion_cardinal_curve_keys(kg, sd->object, sd->prim, sd->time, ka, k0, k1, kb, P_curve);
    }

    float3 p[4];
    p[0] = float4_to_float3(P_curve[0]);
    p[1] = float4_to_float3(P_curve[1]);
    p[2] = float4_to_float3(P_curve[2]);
    p[3] = float4_to_float3(P_curve[3]);

    P = P + D * t;

#  ifdef __UV__
    sd->u = isect->u;
    sd->v = 0.0f;
#  endif

    tg = normalize(curvetangent(isect->u, p[0], p[1], p[2], p[3]));

    if (kernel_data.curve.curveflags & CURVE_KN_RIBBONS) {
      sd->Ng = normalize(-(D - tg * (dot(tg, D))));
    }
    else {
#  ifdef __EMBREE__
      if (kernel_data.bvh.scene) {
        sd->Ng = normalize(isect->Ng);
      }
      else
#  endif
      {
        /* direction from inside to surface of curve */
        float3 p_curr = curvepoint(isect->u, p[0], p[1], p[2], p[3]);
        sd->Ng = normalize(P - p_curr);

        /* adjustment for changing radius */
        float gd = isect->v;

        if (gd != 0.0f) {
          sd->Ng = sd->Ng - gd * tg;
          sd->Ng = normalize(sd->Ng);
        }
      }
    }

    /* todo: sometimes the normal is still so that this is detected as
     * backfacing even if cull backfaces is enabled */

    sd->N = sd->Ng;
  }
  else {
    float4 P_curve[2];

    if (sd->type & PRIMITIVE_CURVE) {
      P_curve[0] = kernel_tex_fetch(__curve_keys, k0);
      P_curve[1] = kernel_tex_fetch(__curve_keys, k1);
    }
    else {
      motion_curve_keys(kg, sd->object, sd->prim, sd->time, k0, k1, P_curve);
    }

    float l = 1.0f;
    tg = normalize_len(float4_to_float3(P_curve[1] - P_curve[0]), &l);

    P = P + D * t;

    float3 dif = P - float4_to_float3(P_curve[0]);

#  ifdef __UV__
    sd->u = dot(dif, tg) / l;
    sd->v = 0.0f;
#  endif

    if (flag & CURVE_KN_TRUETANGENTGNORMAL) {
      sd->Ng = -(D - tg * dot(tg, D));
      sd->Ng = normalize(sd->Ng);
    }
    else {
      float gd = isect->v;

      /* direction from inside to surface of curve */
      float denom = fmaxf(P_curve[0].w + sd->u * l * gd, 1e-8f);
      sd->Ng = (dif - tg * sd->u * l) / denom;

      /* adjustment for changing radius */
      if (gd != 0.0f) {
        sd->Ng = sd->Ng - gd * tg;
      }

      sd->Ng = normalize(sd->Ng);
    }

    sd->N = sd->Ng;
  }

#  ifdef __DPDU__
  /* dPdu/dPdv */
  sd->dPdu = tg;
  sd->dPdv = cross(tg, sd->Ng);
#  endif

  if (isect->object != OBJECT_NONE) {
#  ifdef __OBJECT_MOTION__
    Transform tfm = sd->ob_tfm;
#  else
    Transform tfm = object_fetch_transform(kg, isect->object, OBJECT_TRANSFORM);
#  endif

    P = transform_point(&tfm, P);
  }

  return P;
}

#endif

CCL_NAMESPACE_END
