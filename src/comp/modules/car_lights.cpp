#include "std_include.hpp"
#include "car_lights.hpp"

#include "shared/common/remix_api.hpp"

namespace comp
{
	namespace
	{
		std::vector<car_lights::flare_sample> g_flares;
		std::vector<car_lights::car_sample> g_cars;
		std::vector<car_lights::car_sample> g_cars_fb;
		std::vector<remixapi_LightHandle> g_handles;
		car_lights::stats_t g_stats_building;

		struct vertex_layout
		{
			int pos_offset = -1;
			int col_offset = -1;
			bool valid = false;
		};

		vertex_layout get_layout(IDirect3DDevice9* dev)
		{
			vertex_layout l{};

			IDirect3DVertexDeclaration9* decl = nullptr;
			if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl)
			{
				D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1] = {};
				UINT n = 0;
				if (SUCCEEDED(decl->GetDeclaration(elems, &n)))
				{
					for (UINT i = 0; i < n; i++)
					{
						const auto& e = elems[i];
						if (e.Stream == 0xFF) break;
						if (e.Stream != 0) continue;

						if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0
							&& (e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4)) {
							l.pos_offset = e.Offset;
						}
						else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_D3DCOLOR) {
							l.col_offset = e.Offset;
						}
					}
				}
				decl->Release();
			}
			else
			{
				DWORD fvf = 0;
				if (SUCCEEDED(dev->GetFVF(&fvf)) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ)
				{
					l.pos_offset = 0;
					int off = 12;
					if (fvf & D3DFVF_NORMAL) off += 12;
					if (fvf & D3DFVF_PSIZE) off += 4;
					if (fvf & D3DFVF_DIFFUSE) l.col_offset = off;
				}
			}

			l.valid = l.pos_offset >= 0;
			return l;
		}

		float len(const D3DXVECTOR3& v) { return D3DXVec3Length(&v); }
	}

	void car_lights::collect(IDirect3DDevice9* dev, const uint8_t* base, UINT count, UINT stride, bool from_vb)
	{
		const auto layout = get_layout(dev);
		if (!layout.valid || !base || stride == 0 || count < 4)
		{
			g_stats_building.decl_fail++;
			return;
		}

		D3DXMATRIX world;
		if (FAILED(dev->GetTransform(D3DTS_WORLD, &world))) {
			D3DXMatrixIdentity(&world);
		}

		// los flares son quads: 4 vertices consecutivos por flare
		for (UINT q = 0; q + 3 < count; q += 4)
		{
			D3DXVECTOR3 c(0, 0, 0);
			D3DXVECTOR3 col(0, 0, 0);
			float a = 0.0f;

			for (UINT k = 0; k < 4; k++)
			{
				const uint8_t* v = base + (q + k) * stride;
				const float* p = reinterpret_cast<const float*>(v + layout.pos_offset);
				c += D3DXVECTOR3(p[0], p[1], p[2]);

				if (layout.col_offset >= 0)
				{
					const DWORD argb = *reinterpret_cast<const DWORD*>(v + layout.col_offset);
					col += D3DXVECTOR3(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f);
					a = (std::max)(a, ((argb >> 24) & 0xFF) / 255.0f);
				}
				else
				{
					col += D3DXVECTOR3(1, 1, 1);
					a = 1.0f;
				}
			}

			c *= 0.25f;
			col *= 0.25f;

			D3DXVECTOR3 wpos;
			D3DXVec3TransformCoord(&wpos, &c, &world);

			// evita duplicados (mismo flare dibujado dos veces)
			bool dup = false;
			for (const auto& f : g_flares)
			{
				D3DXVECTOR3 d = f.pos - wpos;
				if (len(d) < 0.05f) { dup = true; break; }
			}
			if (dup) continue;

			g_flares.push_back({ wpos, col, a });

			if (from_vb) g_stats_building.flares_vb++;
			else g_stats_building.flares_up++;
		}
	}

	void car_lights::on_flare_draw_up(IDirect3DDevice9* dev, UINT min_vertex, UINT num_vertices, const void* vertex_data, UINT stride)
	{
		if (!settings().enabled || !vertex_data) return;
		collect(dev, static_cast<const uint8_t*>(vertex_data) + min_vertex * stride, num_vertices, stride, false);
	}

	void car_lights::on_flare_draw_vb(IDirect3DDevice9* dev, INT base_vertex, UINT min_vertex, UINT num_vertices)
	{
		if (!settings().enabled) return;

		IDirect3DVertexBuffer9* vb = nullptr;
		UINT offset = 0, stride = 0;
		if (FAILED(dev->GetStreamSource(0, &vb, &offset, &stride)) || !vb || !stride)
		{
			if (vb) vb->Release();
			g_stats_building.vb_lock_fail++;
			return;
		}

		void* data = nullptr;
		const UINT start = offset + static_cast<UINT>(base_vertex + static_cast<INT>(min_vertex)) * stride;
		if (SUCCEEDED(vb->Lock(start, num_vertices * stride, &data, D3DLOCK_READONLY)) && data)
		{
			collect(dev, static_cast<const uint8_t*>(data), num_vertices, stride, true);
			vb->Unlock();
		}
		else {
			g_stats_building.vb_lock_fail++;
		}

		vb->Release();
	}

	void car_lights::on_car_draw(const D3DXMATRIX* m, bool fallback)
	{
		if (!settings().enabled || !m) return;
		auto& list = fallback ? g_cars_fb : g_cars;

		D3DXVECTOR3 pos(m->_41, m->_42, m->_43);
		D3DXVECTOR3 fwd(m->_11, m->_12, m->_13);
		if (len(fwd) < 0.001f) return;
		D3DXVec3Normalize(&fwd, &fwd);

		for (const auto& c : list)
		{
			D3DXVECTOR3 d = c.pos - pos;
			if (len(d) < (fallback ? 1.0f : 0.5f)) return; // misma carroceria (varias piezas)
		}

		list.push_back({ pos, fwd });
	}

	void car_lights::frame()
	{
		auto& api = shared::common::remix_api::get();
		if (!shared::common::remix_api::is_initialized())
		{
			g_flares.clear(); g_cars.clear(); g_cars_fb.clear();
			return;
		}

		for (auto h : g_handles) {
			if (h) api.m_bridge.DestroyLight(h);
		}
		g_handles.clear();

		const auto& s = settings();
		auto st = g_stats_building;
		st.cars = static_cast<int>(g_cars.size());
		st.cars_fallback = static_cast<int>(g_cars_fb.size());
		const bool use_fb = g_cars.empty();
		const auto& cars = use_fb ? g_cars_fb : g_cars;
		st.first_count = (std::min)(6, static_cast<int>(g_flares.size()));
		for (int i = 0; i < st.first_count; i++) st.first[i] = g_flares[i];

		if (s.enabled)
		{
			D3DXVECTOR3 cam(0, 0, 0);
			if (game::the_camera) {
				cam = D3DXVECTOR3(game::the_camera->position.x, game::the_camera->position.y, game::the_camera->position.z);
			}

			// mas cercanos primero
			std::sort(g_flares.begin(), g_flares.end(), [&](const flare_sample& a, const flare_sample& b)
			{
				D3DXVECTOR3 da = a.pos - cam, db = b.pos - cam;
				return len(da) < len(db);
			});

			int created = 0;
			for (const auto& f : g_flares)
			{
				if (created >= s.max_lights) break;

				D3DXVECTOR3 dc = f.pos - cam;
				if (len(dc) > s.max_dist) { st.skipped++; continue; }

				// carro mas cercano
				const car_sample* car = nullptr;
				float best = s.car_match_dist;
				for (const auto& c : cars)
				{
					D3DXVECTOR3 d = f.pos - c.pos;
					const float dl = len(d);
					if (dl < best) { best = dl; car = &c; }
				}

				const bool is_red = f.color.x > 0.35f && f.color.x > f.color.y * 1.6f && f.color.x > f.color.z * 1.6f;

				remixapi_LightInfoSphereEXT ext = {};
				ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				ext.pNext = nullptr;
				ext.position = { f.pos.x, f.pos.y, f.pos.z };
				ext.volumetricRadianceScale = 1.0f;
				ext.shaping_hasvalue = FALSE;

				remixapi_LightInfo info = {};
				info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				info.pNext = &ext;
				info.hash = shared::utils::string_hash64(shared::utils::va("tn_carlight_%d", created));

				if (car && !is_red)
				{
					// solo faros delanteros (los blancos de atras = reversa)
					D3DXVECTOR3 rel = f.pos - car->pos;
					if (!s.headlights || (!use_fb && D3DXVec3Dot(&rel, &car->fwd) <= 0.0f)) { st.skipped++; continue; }

					D3DXVECTOR3 dir = car->fwd;
					if (use_fb)
					{
						// sin carroceria fiable: apuntar desde el centro del carro hacia el faro (horizontal)
						dir = rel; dir.z = 0.0f;
						if (len(dir) < 0.01f) dir = car->fwd;
						D3DXVec3Normalize(&dir, &dir);
					}
					// inclinar un poco hacia el suelo (eje Z arriba en NFSC)
					const float pitch = D3DXToRadian(s.head_pitch_down_deg);
					dir.z -= tanf(pitch);
					D3DXVec3Normalize(&dir, &dir);

					ext.radius = s.head_radius;
					ext.shaping_hasvalue = TRUE;
					ext.shaping_value.direction = { dir.x, dir.y, dir.z };
					ext.shaping_value.coneAngleDegrees = s.head_cone_deg;
					ext.shaping_value.coneSoftness = s.head_softness;
					ext.shaping_value.focusExponent = 0.0f;

					// color calido-blanco a partir del flare
					D3DXVECTOR3 c = f.color * 0.3f + D3DXVECTOR3(0.7f, 0.7f, 0.68f);
					info.radiance = { c.x * s.head_intensity, c.y * s.head_intensity, c.z * s.head_intensity };
					st.head++;
				}
				else if (car && is_red)
				{
					if (!s.rearlights) { st.skipped++; continue; }

					float k = s.rear_intensity * (std::max)(0.15f, f.alpha);
					if (f.alpha >= s.brake_alpha_threshold) k *= s.brake_boost;

					ext.radius = s.rear_radius;
					info.radiance = { 1.0f * k, 0.05f * k, 0.03f * k };
					st.rear++;
				}
				else
				{
					if (!s.street_flares) { st.skipped++; continue; }

					ext.radius = 0.1f;
					const float k = s.rear_intensity * 2.0f * f.alpha;
					info.radiance = { f.color.x * k, f.color.y * k, f.color.z * k };
					st.street++;
				}

				remixapi_LightHandle h = nullptr;
				if (api.m_bridge.CreateLight(&info, &h) == REMIXAPI_ERROR_CODE_SUCCESS && h)
				{
					api.m_bridge.DrawLightInstance(h);
					g_handles.push_back(h);
					created++;
				}
			}
		}

		last_stats() = st;
		g_stats_building = {};
		g_flares.clear();
		g_cars.clear();
		g_cars_fb.clear();
	}
}
