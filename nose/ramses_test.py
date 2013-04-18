import pynbody

def setup() :
    global f
    f = pynbody.load("testdata/ramses_partial_output_00250")


def test_lengths() :
    assert len(f.gas)==152574
    assert len(f.star)==2655
    assert len(f.dm)==51887


def close_enough(x,y) :
    return abs(x-y)<1.e-5

def test_array_unit_sanity() :
    # test designed to pick up on problems with converting
    # arrays as they get promoted from family to simulation level
    
    f.gas['pos']
    f.star['pos']
    f.dm['pos']
    f.physical_units()

    f2 = pynbody.load("testdata/ramses_partial_output_00250")
    f2.physical_units()
    f2.gas['pos']
    f2.dm['pos']
    f2.star['pos']
    
    
    
    
    print "OK for stars,gas,dm:"
    
    print close_enough(f2.star['pos'],f.star['pos']).all(), \
        close_enough(f2.gas['pos'],f.gas['pos']).all(), \
        close_enough(f2.dm['pos'],f.dm['pos']).all()
    
    assert close_enough(f2['pos'],f['pos']).all()
