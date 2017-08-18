package cn.edu.ruc.iir.pixels.core.stats;

import cn.edu.ruc.iir.pixels.core.PixelsProto;

/**
 * pixels
 *
 * @author guodong
 */
public class IntegerStatsRecorder extends StatsRecorder implements IntegerColumnStats
{
    private long minimum = Long.MIN_VALUE;
    private long maximum = Long.MAX_VALUE;
    private long sum = 0L;
    private boolean hasMinimum = false;
    private boolean overflow = false;

    IntegerStatsRecorder() {}

    IntegerStatsRecorder(PixelsProto.ColumnStatistic statistic)
    {
        super(statistic);
        PixelsProto.IntegerStatistic intStat = statistic.getIntStatistics();
        if (intStat.hasMinimum()) {
            hasMinimum = true;
            minimum = intStat.getMinimum();
        }
        if (intStat.hasMaximum()) {
            maximum = intStat.getMaximum();
        }
        if (intStat.hasSum()) {
            sum = intStat.getSum();
        }
        else {
            overflow = true;
        }
    }

    @Override
    public void reset()
    {
        super.reset();
        hasMinimum = false;
        minimum = Long.MIN_VALUE;
        maximum = Long.MAX_VALUE;
        sum = 0L;
        overflow = false;
    }

    @Override
    public void updateInteger(long value, int repetitions)
    {
        if (!hasMinimum) {
            hasMinimum = true;
            minimum = value;
            maximum = value;
        }
        else if (value < minimum) {
            minimum = value;
        }
        else if (value > maximum) {
            maximum = value;
        }
        if (!overflow) {
            boolean wasPositive = sum >= 0;
            sum += value * repetitions;
            if ((value >= 0) == wasPositive) {
                overflow = (sum >= 0) != wasPositive;
            }
        }
    }

    @Override
    public void merge(StatsRecorder other)
    {
        if (other instanceof IntegerStatsRecorder) {
            IntegerStatsRecorder intStat = (IntegerStatsRecorder) other;
            if (!hasMinimum) {
                hasMinimum = intStat.hasMinimum;
                minimum = intStat.minimum;
                maximum = intStat.maximum;
            }
            else if (intStat.hasMinimum) {
                if (intStat.minimum < minimum) {
                    minimum = intStat.minimum;
                }
                if (intStat.maximum > maximum) {
                    maximum = intStat.maximum;
                }
            }

            overflow |= intStat.overflow;
            if (!overflow) {
                boolean wasPositive = sum >= 0;
                sum += intStat.sum;
                if ((intStat.sum >= 0) == wasPositive) {
                    overflow = (sum >= 0) != wasPositive;
                }
            }
        }
        else {
            if (isStatsExists() && hasMinimum) {
                throw new IllegalArgumentException("Incompatible merging of integer column statistics");
            }
        }
        super.merge(other);
    }

    @Override
    public PixelsProto.ColumnStatistic.Builder serialize()
    {
        PixelsProto.ColumnStatistic.Builder builder = super.serialize();
        PixelsProto.IntegerStatistic.Builder intBuilder =
                PixelsProto.IntegerStatistic.newBuilder();
        if (hasMinimum) {
            intBuilder.setMinimum(minimum);
            intBuilder.setMaximum(maximum);
        }
        if (!overflow) {
            intBuilder.setSum(sum);
        }
        builder.setIntStatistics(intBuilder);
        return builder;
    }

    @Override
    public long getMinimum()
    {
        return minimum;
    }

    @Override
    public long getMaximum()
    {
        return maximum;
    }

    @Override
    public boolean isSumDefined()
    {
        return !overflow;
    }

    @Override
    public long getSum()
    {
        return sum;
    }

    @Override
    public String toString() {
        StringBuilder buf = new StringBuilder(super.toString());
        if (hasMinimum) {
            buf.append(" min: ");
            buf.append(minimum);
            buf.append(" max: ");
            buf.append(maximum);
        }
        if (!overflow) {
            buf.append(" sum: ");
            buf.append(sum);
        }
        return buf.toString();
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (!(o instanceof IntegerStatsRecorder)) {
            return false;
        }
        if (!super.equals(o)) {
            return false;
        }

        IntegerStatsRecorder that = (IntegerStatsRecorder) o;

        if (minimum != that.minimum) {
            return false;
        }
        if (maximum != that.maximum) {
            return false;
        }
        if (sum != that.sum) {
            return false;
        }
        if (hasMinimum != that.hasMinimum) {
            return false;
        }
        if (overflow != that.overflow) {
            return false;
        }

        return true;
    }

    @Override
    public int hashCode() {
        int result = super.hashCode();
        result = 31 * result + (int) (minimum ^ (minimum >>> 32));
        result = 31 * result + (int) (maximum ^ (maximum >>> 32));
        result = 31 * result + (int) (sum ^ (sum >>> 32));
        result = 31 * result + (hasMinimum ? 1 : 0);
        result = 31 * result + (overflow ? 1 : 0);
        return result;
    }

}