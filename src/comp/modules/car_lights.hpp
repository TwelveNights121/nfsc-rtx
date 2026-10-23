#pragma once

// TwelveNights: luces reales (Remix API) para los faros y stops de TODOS los carros.
// Usa los "flares" que el juego ya dibuja en cada luz de carro para saber DONDE va la luz,
// y la matriz del carro mas cercano (CarShader) para saber HACIA DONDE apunta el faro.

namespace comp
{
	class car_lights
	{
	public:
		struct settings_t
		{
			bool  enabled = true;
			bool  headlights = true;
			bool  rearlights = true;
			bool  street_flares = false;		// flares lejos de cualquier carro (postes, etc.)
			float head_intensity = 250.0f;		// igual que los faros del mod (inputs:intensity = 250)
			float head_cone_deg = 55.0f;
			float head_softness = 0.2f;
			float head_radius = 0.05f;
			float head_pitch_down_deg = 3.0f;
			float rear_intensity = 12.0f;
			float brake_boost = 4.0f;			// multiplicador cuando el flare rojo es intenso (frenando)
			float brake_alpha_threshold = 0.6f;
			float rear_radius = 0.04f;
			float car_match_dist = 4.5f;		// metros entre flare y centro del carro
			float max_dist = 120.0f;			// distancia maxima a la camara
			int   max_lights = 48;
		};

		struct flare_sample { D3DXVECTOR3 pos; D3DXVECTOR3 color; float alpha; };
		struct car_sample { D3DXVECTOR3 pos; D3DXVECTOR3 fwd; };

		struct stats_t
		{
			int flares_up = 0, flares_vb = 0, vb_lock_fail = 0, decl_fail = 0;
			int cars = 0, cars_fallback = 0, head = 0, rear = 0, street = 0, skipped = 0;
			flare_sample first[6] = {};
			int first_count = 0;
		};

		static settings_t& settings() { static settings_t s; return s; }
		static stats_t& last_stats() { static stats_t s; return s; }

		// llamado desde el renderer en cada draw de la tecnica FLARES
		static void on_flare_draw_up(IDirect3DDevice9* dev, UINT min_vertex, UINT num_vertices, const void* vertex_data, UINT stride);
		static void on_flare_draw_vb(IDirect3DDevice9* dev, INT base_vertex, UINT min_vertex, UINT num_vertices);

		// llamado desde el renderer cuando dibuja la carroceria de un carro
		static void on_car_draw(const D3DXMATRIX* local_to_world, bool fallback = false);

		// una vez por cuadro (on_begin_scene_cb): crea las luces del cuadro anterior
		static void frame();

	private:
		static void collect(IDirect3DDevice9* dev, const uint8_t* base, UINT count, UINT stride, bool from_vb);
	};
}
