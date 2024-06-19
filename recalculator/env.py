import random
from typing import Optional, Union, List, Tuple
import socket

import logging
import struct
import gym
import numpy as np
from gym.core import RenderFrame, ActType, ObsType
from gym.spaces import Discrete, Box, Sequence
from gym import Env

logger = logging.getLogger()


class NginxEnv(Env):
    def __init__(self):
        # self.upstream = Upstream(random.randint(1, 10), 10, 100)

        self._max_episode_steps = 100
        self.action_space = Box(low=0, high=1, shape=(5,))
        self.observation_space = Box(0, 100, shape=(15,))
        self.state = self.upstream.step(self.action_space.sample()).state
        self.max_steps = 10

    @staticmethod
    def learning_handler(client_socket, action):

        # cnt_servers = len(observation) // 3
        # action = test_model.select_action(np.array(translate_neuro_weights(observation, 5)))
        # processed_data = translate_neuro_weights(action, cnt_servers)
        # logger.info(f"Processed data: {processed_data}")

        # Подготовка и отправка ответа
        response_data = np.array(action, dtype=np.float32).tobytes()
        client_socket.sendall(response_data)
        logger.info("Response sent")

        data_length = struct.unpack('I', client_socket.recv(4))[0]
        logger.info(f"Received data length: {data_length}")
        data = client_socket.recv(data_length)
        logger.info(f"Received data: {data}")
        observation = np.frombuffer(data, dtype=np.int32)
        logger.info(f"Converted observation: {observation}")


        # Закрытие соединения
        client_socket.close()
        logger.info("Connection closed")
        return observation

    @staticmethod
    def start_server(action):
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.bind(('127.0.0.1', 7998))
        server_socket.listen(1)

        logger.info("Server is listening on port 7998...")

        client_socket, client_address = server_socket.accept()
        logger.info(f"Connection from {client_address}")

        NginxEnv.learning_handler(client_socket, action)

        server_socket.close()
        logger.info("Server has been stopped")

    def step(self, action):
        self.max_steps -= 1

        # callback = self.upstream.step(action)

        # reward = callback.reward

        if self.max_steps == 0:
            done = True
        else:
            done = False

        # self.state = callback.state

        # return self.state, reward, done, {}

    def render(self) -> Optional[Union[RenderFrame, List[RenderFrame]]]:
        pass

    def reset(self, *, seed: Optional[int] = None, options: Optional[dict] = None):
        # self.upstream = Upstream(random.randint(1, 10), 10, 100)
        # self.state = self.upstream.step(self.action_space.sample()).state
        # self.max_steps = 10

        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.bind(('127.0.0.1', 7998))
        server_socket.listen(1)


        return self.state
