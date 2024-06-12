def translate_neuro_weights(weights: list[int], server_count: int) -> list[float]:
    general_size = len(weights) / server_count
    servers = [0 for _ in range(server_count)]

    server_idx = 0
    size = general_size
    for weight_idx in range(len(weights)):
        integrity = 1.0
        while integrity > 0:
            if size >= integrity:
                servers[server_idx] += integrity * weights[weight_idx]
                size -= integrity
                break
            else:
                servers[server_idx] += size * weights[weight_idx]
                integrity -= size
                size = general_size
                server_idx += 1
                if server_idx >= server_count:
                    break

    return servers
