import asyncio
import struct
import logging
import copy
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from recalculator.config import getenv
from recalculator.misc import translate_neuro_weights
from recalculator.train.TD3 import TD3

device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

async def handler(reader, writer):
    data_length = struct.unpack('I', await reader.read(4))[0]
    logger.info(f"Received data length: {data_length}")
    data = await reader.read(data_length)
    logger.info(f"Received data: {data}")
    observation = np.frombuffer(data, dtype=np.int32)
    logger.info(f"Converted observation: {observation}")
    
    cnt_servers = len(observation) // 2
    action = test_model.select_action(np.array(translate_neuro_weights(observation, 200)))
    processed_data = translate_neuro_weights(action, cnt_servers)
    logger.info(f"Processed data: {processed_data}")
    
    response_data = np.array(processed_data, dtype=np.float32).tobytes()
    writer.write(response_data)
    await writer.drain()
    writer.close()
    await writer.wait_closed()
    logger.info("Response sent and connection closed")


async def main():
    server = await asyncio.start_server(handler, 'recalculator', 7998)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    learning = getenv("LEARNING")
    if learning == "True":
        start_learning = True
    else:
        start_learning = False

    logger.info("Initializing model")
    test_model = TD3(200, 100, 100)
    logger.info("Loading model")
    test_model.load("./weights/TD3_NGinxEnv_0")
    logger.info("Server started")
    asyncio.run(main())
