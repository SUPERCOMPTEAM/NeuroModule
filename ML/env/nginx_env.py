import random
from typing import Optional, Union, List, Tuple

import gym
import numpy as np
from gym.core import RenderFrame, ActType, ObsType
from gym.spaces import Discrete, Box, Sequence
from gym import Env

from models import Upstream


class NginxEnv(Env):
    def __init__(self):
        self.upstream = Upstream(random.randint(1, 10), 10, 100)

        self._max_episode_steps = 100
        self.action_space = Box(low=0, high=1, shape=(100,))
        self.observation_space = Box(0, 100, shape=(200,))
        self.state = self.upstream.step(self.action_space.sample()).state
        self.max_steps = 100

    def step(self, action):
        self.max_steps -= 1

        callback = self.upstream.step(action)

        reward = callback.reward

        if self.max_steps == 0:
            done = True
        else:
            done = False

        self.state = callback.state

        return self.state, reward, done, {}

    def render(self) -> Optional[Union[RenderFrame, List[RenderFrame]]]:
        pass

    def reset(self, *, seed: Optional[int] = None, options: Optional[dict] = None):
        self.upstream = Upstream(random.randint(1, 10), 10, 100)
        self.state = self.upstream.step(self.action_space.sample()).state
        self.max_steps = 100

        return self.state
