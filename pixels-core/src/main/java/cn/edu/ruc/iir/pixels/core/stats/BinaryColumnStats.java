package cn.edu.ruc.iir.pixels.core.stats;

/**
 * pixels
 *
 * @author guodong
 */
public interface BinaryColumnStats extends ColumnStats
{
    /**
     * Get the total length of the binary blob
     * @return sum
     * */
    long getSum();
}