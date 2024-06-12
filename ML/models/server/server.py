import random

from models.request.request import Request


class Server:
    def __init__(self, speed: float, safety: float) -> None:
        self.requests: list[Request] = []
        self.solved_requests: list[Request] = []
        self.unsolved_requests: list[Request] = []

        self.speed: float = speed
        self.safety: float = safety
        self.active: bool = True
        self.overtime: bool = False

    def __str__(self) -> str:
        res = "Solved:\n"
        for request in self.solved_requests:
            res += "\t" + str(request) + "\n"
        res += "Unsolved:\n"
        for request in self.unsolved_requests:
            res += "\t" + str(request) + "\n"
        return res

    def add_request(self, req: Request) -> None:
        self.requests.append(req)

    def process_requests(self, max_time: float) -> tuple[int, int]:
        if len(self.requests) == 0:
            return 0, 0
        sorted_requests = sorted(self.requests, key=lambda x: x.start_time)

        start_time: float = float(sorted_requests[0].start_time)
        current_time: float = float(sorted_requests[0].start_time)
        for request in sorted_requests:
            if random.random() > self.safety:
                self.active = False

            if max_time < current_time - start_time:
                self.overtime = True

            if not self.active and not self.overtime:
                request.cancel()
                self.unsolved_requests.append(request)
            elif not self.overtime:
                request.wait(current_time - request.start_time)
                request.solve(self.speed)
                self.solved_requests.append(request)
            else:
                request.ignore()
                self.unsolved_requests.append(request)

            current_time = float(request.end_time)

        if self.solved_requests:
            if self.solved_requests[-1].end_time > start_time + max_time:
                self.solved_requests[-1].cancel()
                self.unsolved_requests.append(self.solved_requests.pop())

        return len(self.requests), len(self.solved_requests)
