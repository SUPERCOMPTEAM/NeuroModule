import random

from models.request.request import Request
from models.server.server import Server
from models.upstream.upstream_callback import UpstreamCallback
from utils.list_resizer import translate_neuro_weights


def generate_request_list(count: int) -> list[Request]:
    solve_times = [
        random.randint(1, 10),
        random.randint(1, 10),
        random.randint(1, 10),
        random.randint(1, 10)
    ]

    unsorted_request_list = [
        Request(
            random.randint(0, count * 100),
            random.choice(solve_times)
        ) for _ in range(count)
    ]

    return sorted(unsorted_request_list, key=lambda x: x.start_time)


class Upstream:
    def __init__(self, server_count: int, request_limit: int, state_size: int) -> None:
        self.request_limit = request_limit
        self.state_size = state_size

        self.server_list: list[Server] = [
            Server(
                speed=random.uniform(0.5, 2),
                safety=random.uniform(0.98, 1),
            ) for _ in range(server_count)
        ]

    def step(self, weights: list[int]) -> UpstreamCallback:
        server_weights = translate_neuro_weights(weights, len(self.server_list))
        request_list = generate_request_list(self.request_limit)

        total_weight = sum(server_weights)
        cumulative_weights = [sum(server_weights[:i + 1]) for i in range(len(server_weights))]

        for request in request_list:
            rand_value = random.uniform(0, total_weight)

            choose = 0
            for i, weight in enumerate(cumulative_weights):
                if rand_value <= weight:
                    choose = i
                    break

            self.server_list[choose].add_request(request)

        callbacks = []
        for i, server in enumerate(self.server_list):
            max_time = 0
            if len(server.requests) > 0:
                max_time = server.requests[len(server.requests) - 1].start_time + 5
            callbacks.append(server.process_requests(max_time))

        return UpstreamCallback(callbacks, self.state_size)



