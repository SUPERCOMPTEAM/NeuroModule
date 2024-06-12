from enum import Enum


class RequestStatus(Enum):
    SolveRequest = 1
    DontSolveRequest = 3
    BadRequest = 2


class Request:
    def __init__(self, start_time: float, solve_time: float) -> None:
        self.start_time: float = start_time
        self.solve_time: float = solve_time
        self.wait_time: float = 0
        self.end_time: float = 0
        self.status = RequestStatus.DontSolveRequest

    def wait(self, wait_time: float) -> None:
        if wait_time < 0:
            wait_time = 0
        self.wait_time += wait_time

    def solve(self, speed: float) -> None:
        self.status = RequestStatus.SolveRequest
        self.end_time = self.start_time + self.wait_time + (self.solve_time / speed)

    def cancel(self):
        self.status = RequestStatus.BadRequest
        self.end_time = self.start_time

    def ignore(self):
        self.status = RequestStatus.DontSolveRequest
        self.end_time = self.start_time

    def get_start_solve_time(self) -> float:
        return self.start_time + self.wait_time

    def __str__(self) -> str:
        end_time_str = str(self.end_time) if self.end_time else "Not solved yet"
        return f"{self.status.name} - {self.start_time} - {self.solve_time} - {self.wait_time} - {end_time_str}"
