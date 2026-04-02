1. BitRate (The Speed of the Train)

What it is: How many bits (1s and 0s) are sent per second. 100.0 kbps = 100,000 bits per second. The Challenge: The faster you send your 1s and 0s, the "fuzzier" the train gets. High-speed signals naturally spread out and blur together in the air. If you send data extremely fast, the difference between a 1 and a 0 becomes harder to spot at a distance.

2. Frequency_Deviation (The Width of the Track)

What it is: How far apart the 1 signal and the 0 signal are pushed away from your Center Frequency. The Relationship: Because high-speed (BitRate) signals blur together, you have to push the 1 and the 0 further apart so the receiver doesn't mix them up.

The Golden Rule: For FSK radios like the CC1101, mathematicians found that the optimal distance to safely push the 1 and 0 apart is half of the BitRate.
Math: If BitRate = 100.0, then the optimal Frequency_Deviation is 50.0. (Your older value 47.6 was just the closest exact match the CC1101's internal math registers could perfectly generate out of 50.0!)

3. RxBandwidth (The Width of the Tunnel)

What it is: This is the size of the "listening window" or "tunnel" on the receiving walkie-talkie. It has to be wide enough to hear both the high tone (the 1) and the low tone (the 0), while still letting the high-speed "train" pass through cleanly. The Relationship: To calculate exactly how wide the receiving tunnel needs to be, you must use Carsons' Rule: 

RxBandwidth = (2 × Deviation) + BitRate.

It is referenced to the center frequency

