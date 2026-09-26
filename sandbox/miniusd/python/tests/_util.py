import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
DATA = os.path.join(HERE, "data")
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
MODELS = os.path.join(REPO, "models")
