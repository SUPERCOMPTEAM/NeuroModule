from utils.list_resizer import translate_neuro_weights


class UpstreamCallback:
    reward: float = 0
    state: list[float] = []

    def __init__(self, server_callbacks: list[tuple[int, int]], state_size: int) -> None:
        self.solve_reward(server_callbacks)
        self.solve_state(server_callbacks, state_size)

    def solve_reward(self, server_callbacks: list[tuple[int, int]]):
        sum_ans = 0
        sum_req = 0
        n = len(server_callbacks)
        for server_callback in server_callbacks:
            sum_req += server_callback[0]
            sum_ans += server_callback[1]

        diff = (sum_ans - sum_req) / sum_req
        if diff < 0:
            self.reward = diff
            return

        diff_sq = 0
        for server_callback in server_callbacks:
            diff_sq += (server_callback[0] - sum_req / n) ** 2

        if diff_sq == 0:
            self.reward = n ** 2
            return

        self.reward = n / diff_sq

    def solve_state(self, server_callbacks: list[tuple[int, int]], state_size: int):
        r = []
        a = []
        for callback in server_callbacks:
            r.append(callback[0])
            a.append(callback[1])

        nr = translate_neuro_weights(r, state_size)
        na = translate_neuro_weights(a, state_size)

        self.state = []
        for i in range(state_size):
            self.state.append(nr[i])
            self.state.append(na[i])
